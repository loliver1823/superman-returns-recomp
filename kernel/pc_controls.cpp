// iter-464 controls lane: host glue - input.ini, SDL mouse events, relative-mouse management,
// the per-poll merge called from kernel/stubs.cpp, the harness read-back. docs/PC_CONTROLS.md.
#include "pc_controls.hpp"
#include "pc_controls_map.hpp"
#include "pc_controls_game.hpp"
#include "pc_moves.hpp"
#include "pc_actions.hpp"
#include "host_automation.hpp"
#include "host_input.hpp"
#include "xam_content.hpp"
#include "guest_context.hpp"   // hostWindowFocused
#include "mod_library.hpp"     // iter-465: the mod library overlay takes the mouse
#include "../app/log.hpp"

#include <SDL3/SDL.h>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>

namespace sr::kernel::pccontrols {

namespace fs = std::filesystem;

namespace {
Config g_config;
std::mutex g_cfgMu;   // init() vs the first poll

// main thread -> poll thread (SDL events)
std::atomic<int64_t> g_mouseDx{0}, g_mouseDy{0};
std::atomic<int32_t> g_wheel{0};
std::atomic<uint32_t> g_mouseButtons{0};
// poll thread -> main thread
std::atomic<bool> g_wantRelative{false};
std::atomic<uint8_t> g_ctxPublished{0};
std::atomic<bool> g_promptPlayer{true};

Mapper g_mapper;
uint32_t g_seq = 0;
uint32_t g_footLookPolls = 0;
// Mouse-look scale at sensitivity 1.0: 0.001 rad (0.057 deg) per count - a full turn is about
// 6300 counts (7.9 in at 800 DPI). docs/PC_CONTROLS.md 4.1.
constexpr float kRadPerCount = 0.0010f;
constexpr float kSimDt = 1.0f / 30.0f;   // the fixed sim tick (FGGameSim, docs/HIGH_FRAMERATE.md)
// harness mouse block: last cumulative values consumed
int32_t g_hDx = 0, g_hDy = 0, g_hWheel = 0;
// lane E E5c: the cursor's title click (pulseStart): pad polls of START left, and the steady-clock ms it ends at
std::atomic<int> g_startPulse{0};
std::atomic<int64_t> g_startPulseUntil{0};
int64_t steadyMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}
constexpr uint16_t kXStart = 0x0010;   // XINPUT_GAMEPAD_START (control 4, kCtlStart)
bool g_hSeen = false;

int envTri(const char* name) {
  const char* v = std::getenv(name);
  if (!v || !*v) return -1;
  return v[0] == '0' ? 0 : 1;
}

Keymap keymapOverride(bool* set) {
  const char* v = std::getenv("SR_PC_KEYMAP");
  *set = v && *v;
  if (!*set) return Keymap::Player;
  return (v[0] == 'l' || v[0] == 'L') ? Keymap::LegacyTest : Keymap::Player;
}

std::string keyboardPrompt(hostinput::Control control) {
  namespace HI = hostinput;
  if (!g_promptPlayer.load(std::memory_order_relaxed))
    return HI::defaultPromptInputFor(control);
  const Config& cfg = config();
  const bool gameplay = lastPollWasGameplay();
  uint16_t input = kInNone;
  switch (control) {
    case HI::kLStickUp: input = firstInput(cfg.bindings, kMoveForward); break;
    case HI::kLStickDown: input = firstInput(cfg.bindings, kMoveBack); break;
    case HI::kLStickLeft: input = firstInput(cfg.bindings, kMoveLeft); break;
    case HI::kLStickRight: input = firstInput(cfg.bindings, kMoveRight); break;
    case HI::kRStickUp: case HI::kRStickDown:
    case HI::kRStickLeft: case HI::kRStickRight: {
      if (gameplay && cfg.mouse.look) return "MouseMove";
      const Action actions[] = {kLookUp, kLookDown, kLookLeft, kLookRight};
      input = firstInput(cfg.bindings, actions[control - HI::kRStickUp]);
      break;
    }
    case HI::kLThumb: case HI::kRThumb: return {};
    default: {
      const uint32_t bit = control == HI::kLT ? kPadLT : control == HI::kRT ? kPadRT
                                                                                : HI::controlButtonBit(control);
      if (bit) input = primaryInputFor(bit, gameplay, cfg.bindings);
      break;
    }
  }
  return input == kInNone ? std::string{} : inputName(input);
}
}  // namespace

const Config& config() { return g_config; }

bool lastPollWasGameplay() {
  const auto c = static_cast<Context>(g_ctxPublished.load(std::memory_order_relaxed));
  return c == Context::Gameplay || c == Context::MiniGame;
}

bool enabled() {
  // SR_PC_CONTROLS=0 restores the iter-450S keyboard merge exactly (A/B).
  static const bool v = envTri("SR_PC_CONTROLS") != 0;
  return v;
}

// Lane E E3: the host screens' keymap per keyboard source - the same decision as mergePoll's.
Keymap hostKeymap(bool harnessChannel) {
  if (!enabled()) return Keymap::LegacyTest;
  bool ovSet = false;
  const Keymap ov = keymapOverride(&ovSet);
  if (ovSet) return ov;
  if (harnessChannel)
    return (automation::harnessFlags() & automation::kHarnessFlagPlayerKeymap) ? Keymap::Player : Keymap::LegacyTest;
  return automation::policy().automated ? Keymap::LegacyTest : Keymap::Player;
}

Keymap hostCapKeymap() {
  if (!enabled()) return Keymap::LegacyTest;
  return g_promptPlayer.load(std::memory_order_relaxed) ? Keymap::Player : Keymap::LegacyTest;
}

void init() {
  static bool s_done = false;
  if (s_done || !enabled()) return;
  s_done = true;
  const auto& pol = automation::policy();
  bool overrideSet = false;
  const Keymap overrideMap = keymapOverride(&overrideSet);
  g_promptPlayer.store(overrideSet ? overrideMap == Keymap::Player : !pol.automated,
                      std::memory_order_relaxed);
  hostinput::setKeyPromptSource(keyboardPrompt);
  std::lock_guard<std::mutex> lk(g_cfgMu);
  if (pol.automated) {
    // rolls: always the defaults (the content root is a fresh scratch dir anyway)
    SR_LOG_BOOT(sr::LogSeverity::Info,
                "[PC_CONTROLS] automated run: default layout; harness keymap = %s unless the "
                "channel flags select the player keymap",
                std::getenv("SR_PC_KEYMAP") ? std::getenv("SR_PC_KEYMAP") : "legacy-test");
    return;
  }
  std::error_code ec;
  const fs::path dir = xamContentRoot() / "settings";
  const fs::path file = dir / "input.ini";
  if (!fs::exists(file, ec)) {
    fs::create_directories(dir, ec);
    std::ofstream o(file, std::ios::binary);
    if (o) o << defaultIni();
    SR_LOG_BOOT(sr::LogSeverity::Info, "[PC_CONTROLS] wrote the default %s", file.string().c_str());
    return;
  }
  std::ifstream f(file, std::ios::binary);
  std::stringstream ss;
  ss << f.rdbuf();
  std::string errors;
  g_config = parseIni(ss.str(), Config{}, &errors);
  SR_LOG_BOOT(sr::LogSeverity::Info, "[PC_CONTROLS] read %s (look=%d sensitivity=%.2f)",
              file.string().c_str(), g_config.mouse.look ? 1 : 0, g_config.mouse.sensitivity);
  std::istringstream es(errors);
  std::string line;
  while (std::getline(es, line))
    SR_LOG_BOOT(sr::LogSeverity::Warn, "[PC_CONTROLS] %s", line.c_str());
}

// iter-466 gfx: the PC SETTINGS screen's CONTROLS tab changed input.ini - read it again.
// iter-466 controls2: also on automated runs - the file is the one the screen just wrote in this
// process's own content root (a fresh scratch dir under SR_INSTANCE_ID), so a roll can prove a
// rebind end to end; the start-up read above still keeps rolls on the default layout.
void reload() {
  if (!enabled()) return;
  const fs::path file = xamContentRoot() / "settings" / "input.ini";
  std::ifstream f(file, std::ios::binary);
  if (!f) return;
  std::stringstream ss;
  ss << f.rdbuf();
  std::string errors;
  const Config next = parseIni(ss.str(), Config{}, &errors);
  {
    std::lock_guard<std::mutex> lk(g_cfgMu);
    g_config = next;
  }
  SR_LOG_BOOT(sr::LogSeverity::Info, "[PC_CONTROLS] reloaded %s (look=%d sensitivity=%.2f invert=%d/%d)",
              file.string().c_str(), next.mouse.look ? 1 : 0, next.mouse.sensitivity,
              next.mouse.invertX ? 1 : 0, next.mouse.invertY ? 1 : 0);
}

// ── main thread ───────────────────────────────────────────────────────────────────────
void onSdlEvent(const SDL_Event& e) {
  if (!enabled() || !automation::policy().realInput) return;
  switch (e.type) {
    case SDL_EVENT_MOUSE_MOTION:
      if (relativeMouseOn()) {
        // SDL3 reports motion as float. Relative mode reads raw WM_INPUT counts on Windows (whole
        // numbers with no system scale, SDL_HINT_MOUSE_RELATIVE_SYSTEM_SCALE=0, the default); any
        // fraction is carried here, so the poll still gets whole counts and loses none.
        static float s_fx = 0.0f, s_fy = 0.0f;
        s_fx += e.motion.xrel;
        s_fy += e.motion.yrel;
        const float ix = std::trunc(s_fx), iy = std::trunc(s_fy);
        s_fx -= ix;
        s_fy -= iy;
        g_mouseDx.fetch_add(int64_t(ix), std::memory_order_relaxed);
        g_mouseDy.fetch_add(int64_t(iy), std::memory_order_relaxed);
      }
      break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP: {
      uint32_t bit = 0;
      switch (e.button.button) {
        case SDL_BUTTON_LEFT: bit = 1; break;
        case SDL_BUTTON_RIGHT: bit = 2; break;
        case SDL_BUTTON_MIDDLE: bit = 4; break;
        case SDL_BUTTON_X1: bit = 8; break;
        case SDL_BUTTON_X2: bit = 16; break;
        default: break;
      }
      if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN) g_mouseButtons.fetch_or(bit);
      else g_mouseButtons.fetch_and(~bit);
      break;
    }
    case SDL_EVENT_MOUSE_WHEEL: {
      // SDL3: wheel.y is the float amount (fractional on hi-res wheels); integer_y is SDL2's whole
      // ticks (accumulated the same way).
      int y = e.wheel.integer_y;
      if (e.wheel.direction == SDL_MOUSEWHEEL_FLIPPED) y = -y;
      g_wheel.fetch_add(y, std::memory_order_relaxed);
      break;
    }
    case SDL_EVENT_WINDOW_FOCUS_LOST:
      g_mouseButtons.store(0);
      break;
    default:
      break;
  }
}

// SDL3 documents SDL_GetWindowRelativeMouseMode as main-thread only, and the pad poll (mergePoll) runs on a
// guest thread: everyone reads this copy, which mainThreadTick (the one place the mode is switched)
// publishes after every check of the game window's state.
std::atomic<bool> g_relativeOn{false};
bool relativeMouseOn() { return g_relativeOn.load(std::memory_order_acquire); }

void mainThreadTick(void* sdlWindow) {
  if (!enabled()) return;
  // iter-465 modlib: the mod library overlay is a menu: the cursor, never mouse-look.
  const bool want = g_wantRelative.load(std::memory_order_relaxed) && !modlib::isOpen() &&
                    automation::policy().realInput && hostWindowFocused() && sdlWindow;
  SDL_Window* win = static_cast<SDL_Window*>(sdlWindow);
  const bool is = win && SDL_GetWindowRelativeMouseMode(win);
  if (want != is) {
    // Raw, unaccelerated deltas: relative mode reads WM_INPUT on Windows (SDL3 always; SDL2 did
    // with SDL_HINT_MOUSE_RELATIVE_MODE_WARP=0) and applies no system acceleration
    // (SDL_HINT_MOUSE_RELATIVE_SYSTEM_SCALE=0, the default). SDL3: the mode is per window.
    SDL_SetWindowRelativeMouseMode(win, want);
    g_mouseDx.store(0);
    g_mouseDy.store(0);
    SR_LOG_BOOT(sr::LogSeverity::Info, "[PC_CONTROLS] mouse-look %s (context %u)",
                want ? "ON (relative mouse)" : "OFF (cursor)",
                unsigned(g_ctxPublished.load()));
  }
  g_relativeOn.store(win && SDL_GetWindowRelativeMouseMode(win), std::memory_order_release);
}

// ── the pad poll (kernel/stubs.cpp mergeKeyboardPad) ────────────────────────────────────
uint16_t mergePoll(Pad* pad, const uint8_t* harnessKeys, const uint8_t* realKeys,
                   bool realFocused, bool* anyHeld) {
  if (!enabled()) return 0;
  const auto& pol = automation::policy();
  bool ovSet = false;
  const Keymap ov = keymapOverride(&ovSet);
  const uint32_t hflags = automation::harnessFlags();

  // Which keymap each keyboard source uses: players = PLAYER; automated runs = the TEST
  // keymap unless SR_PC_KEYMAP or the channel flags say player.
  const Keymap realMap = ovSet ? ov : (pol.automated ? Keymap::LegacyTest : Keymap::Player);
  const Keymap harnessMap = ovSet ? ov
                          : ((hflags & automation::kHarnessFlagPlayerKeymap) ? Keymap::Player
                                                                            : Keymap::LegacyTest);
  g_promptPlayer.store(realMap == Keymap::Player || harnessMap == Keymap::Player,
                      std::memory_order_relaxed);

  const uint16_t before = pad->buttons;
  const Pad padIn = *pad;
  uint8_t merged[512];
  bool havePlayerKeys = false;
  std::memset(merged, 0, sizeof merged);
  auto addPlayer = [&](const uint8_t* k) {
    for (int i = 0; i < 512; ++i) merged[i] |= k[i];
    havePlayerKeys = true;
  };
  if (harnessKeys) {
    if (harnessMap == Keymap::LegacyTest) mapLegacyTestKeys(harnessKeys, pad);
    else addPlayer(harnessKeys);
  }
  if (realKeys && realFocused) {
    if (realMap == Keymap::LegacyTest) mapLegacyTestKeys(realKeys, pad);
    else addPlayer(realKeys);
  }

  // the game's own state (observe-only reads; pc_controls_game.cpp)
  const GameView gv = readGameView();
  const Context ctx = gv.ctx;
  g_ctxPublished.store(static_cast<uint8_t>(ctx), std::memory_order_relaxed);
  {
    // evidence: every change of the game's input context / Superman's mode (bounded)
    static int s_ctx = -1, s_mode = -1, s_cfg = -1, s_type = -2;
    static uint32_t s_lines = 0;
    const int mode = gv.air ? (gv.hover ? 2 : 1) : (gv.footCam ? 3 : 0);
    if ((int(ctx) != s_ctx || mode != s_mode || gv.config != s_cfg || gv.igcType != s_type) &&
        s_lines < 400) {
      static const char* const kCtx[] = {"unknown", "menu", "gameplay", "cinematic", "minigame"};
      static const char* const kMode[] = {"-", "flight", "hover", "on-foot camera"};
      ++s_lines;
      SR_LOG_BOOT(sr::LogSeverity::Info,
                  "[PC_CONTROLS] context %s (igc type %d) config %c invX=%d invY=%d mode %s poll=%u",
                  kCtx[static_cast<int>(ctx) % 5], gv.igcType, 'A' + (gv.config & 3),
                  gv.invertX ? 1 : 0, gv.invertY ? 1 : 0, kMode[mode], gv.poll);
      s_ctx = int(ctx);
      s_mode = mode;
      s_cfg = gv.config;
      s_type = gv.igcType;
    }
  }

  PollIn in;
  in.keys = havePlayerKeys ? merged : nullptr;
  in.ctx = ctx;
  in.config = gv.config;
  in.air = gv.air;
  in.dodgeFrame = gv.dodge;
  in.airMoves = pcmoves::enabled();
  // mouse: real (players / realInput) + the harness block
  int32_t dx = 0, dy = 0, wheel = 0;
  uint32_t mb = 0;
  if (pol.realInput) {
    dx = static_cast<int32_t>(g_mouseDx.exchange(0, std::memory_order_relaxed));
    dy = static_cast<int32_t>(g_mouseDy.exchange(0, std::memory_order_relaxed));
    wheel = g_wheel.exchange(0, std::memory_order_relaxed);
    if (realFocused) mb |= g_mouseButtons.load(std::memory_order_relaxed);
  }
  automation::HarnessMouse hm;
  bool harnessMouseLive = false;
  if (automation::harnessMouse(&hm)) {
    harnessMouseLive = true;
    if (!g_hSeen) { g_hDx = hm.dxTotal; g_hDy = hm.dyTotal; g_hWheel = hm.wheelTotal; g_hSeen = true; }
    dx += hm.dxTotal - g_hDx;
    dy += hm.dyTotal - g_hDy;
    wheel += hm.wheelTotal - g_hWheel;
    g_hDx = hm.dxTotal; g_hDy = hm.dyTotal; g_hWheel = hm.wheelTotal;
    mb |= hm.buttons;
  }
  const bool look = g_config.mouse.look && ctx == Context::Gameplay;
  g_wantRelative.store(look, std::memory_order_relaxed);
  const bool lookLive = look && (harnessMouseLive || relativeMouseOn());
  const float sens = g_config.mouse.sensitivity;
  if (lookLive && gv.footCam && footMouseLookEnabled()) {
    // ON FOOT (section 4.2): the camera's own angles, 1:1. The game's sense: a stick pushed
    // right turns yaw down unless X INVERSION is on; pushed up turns pitch down unless Y
    // INVERSION is on - the mouse follows the same options as the stick.
    const float k = kRadPerCount * sens;
    const float hx = g_config.mouse.invertX ? -1.0f : 1.0f;
    const float hy = g_config.mouse.invertY ? -1.0f : 1.0f;
    const float dYaw = float(dx) * k * hx * (gv.invertX ? 1.0f : -1.0f);
    const float dPitch = float(-dy) * k * hy * (gv.invertY ? 1.0f : -1.0f);
    publishFootLook(dYaw, dPitch, gv.poll);
    if (dx || dy) ++g_footLookPolls;
  } else if (lookLive && gv.air && gv.flyYawRate > 0.0f && gv.flyPitchRate > 0.0f) {
    // IN FLIGHT (section 4.3): the stick turns Superman; the stick that turns him by exactly
    // the mouse's angle this tick (the inverse of the game's flight law).
    in.mouseLookActive = true;
    in.lookUnitsPerCountX = kRadPerCount / (gv.flyYawRate * kSimDt);
    in.lookUnitsPerCountY = kRadPerCount / (gv.flyPitchRate * kSimDt);
  }
  in.mouseDx = in.mouseLookActive ? dx : 0;
  in.mouseDy = in.mouseLookActive ? dy : 0;
  in.wheel = wheel;
  in.mouseButtons = mb;

  // iter-466 controls2 (USER DECISION 2026-10-06, docs/PC_CONTROLS.md 2): the player's keys and
  // mouse are the game's ACTIONS, never pad state. The mapper builds this tick's frame of the
  // game's controls the bound actions drive; the action layer (kernel/pc_actions.cpp) feeds it to
  // the game's own input commands after the game's own evaluation. *pad (the XInput report) only
  // ever carries the controller and the harness TEST keymap (test tooling).
  Pad frame;
  PollOut out;
  {
    std::lock_guard<std::mutex> lk(g_cfgMu);
    g_mapper.poll(in, g_config, &frame, &out);
  }
  const bool frameLive = frame.buttons || frame.lt || frame.rt || frame.lx || frame.ly ||
                         frame.rx || frame.ry;
  if (pcactions::enabled()) {
    // lane E E5c: the cursor's title click rides on the published frame only - it is not the player's keyboard
    // (frameLive / anyHeld and the read-back below stay the keys' own)
    Pad pub = frame;
    pub.buttons |= takeStartPulse();
    pcactions::publish(controlFrameOf(pub));
  } else if (frameLive) {
    static bool s_said = false;
    if (!s_said) {
      s_said = true;
      SR_LOG_BOOT(sr::LogSeverity::Warn,
                  "[PC_CONTROLS] player-keymap input ignored: the action layer is off (port features "
                  "off - a natural roll; SR_PORT_FEATURES=1 turns it on)");
    }
  }
  if (out.dodgeAir) pcmoves::requestAirDodge(out.dodgeKeysX, out.dodgeKeysY, gv.poll);
  if (anyHeld) {
    *anyHeld = frameLive || pad->buttons != before || pad->lx != padIn.lx ||
               pad->ly != padIn.ly || pad->rx != padIn.rx || pad->ry != padIn.ry ||
               pad->lt != padIn.lt || pad->rt != padIn.rt;
  }
  // the read-back reports what the player's input drives: the controller / test keymap report
  // with the action frame on top
  Pad view = *pad;
  view.buttons |= frame.buttons;
  if (frame.lt) view.lt = frame.lt;
  if (frame.rt) view.rt = frame.rt;
  if (frame.lx) view.lx = frame.lx;
  if (frame.ly) view.ly = frame.ly;
  if (frame.rx) view.rx = frame.rx;
  if (frame.ry) view.ry = frame.ry;

  // read-back for the harness (docs/PC_CONTROLS.md 8): <BBBBHBBhhhhII>
  uint8_t rb[automation::kHarnessReadbackBytes] = {};
  rb[0] = 1;
  rb[1] = static_cast<uint8_t>(ctx);
  rb[2] = out.dodgePhase;
  rb[3] = static_cast<uint8_t>((gv.air ? 1 : 0) | (gv.valid ? 2 : 0) | (gv.hover ? 4 : 0) |
                               (gv.footCam ? 8 : 0) | (gv.dodge.valid ? 16 : 0) |
                               (static_cast<uint8_t>(pcmoves::lastKind()) << 5));
  std::memcpy(rb + 4, &view.buttons, 2);
  rb[6] = view.lt;
  rb[7] = view.rt;
  std::memcpy(rb + 8, &view.lx, 2);
  std::memcpy(rb + 10, &view.ly, 2);
  std::memcpy(rb + 12, &view.rx, 2);
  std::memcpy(rb + 14, &view.ry, 2);
  ++g_seq;
  std::memcpy(rb + 16, &g_seq, 4);
  static uint32_t s_look = 0;
  s_look += out.mouseLookPolls;
  const uint32_t lookPolls = s_look + g_footLookPolls;
  std::memcpy(rb + 20, &lookPolls, 4);
  automation::harnessWriteReadback(rb, sizeof rb);

  if (out.dodgeRequested) {
    static const char* const kKind[] = {"-", "BACK", "LEFT", "RIGHT"};
    SR_LOG_BOOT(sr::LogSeverity::Info,
                "[PC_CONTROLS] dodge key: ctx=%u air=%d hover=%d keys=(%d,%d) frame=%d -> %s%s "
                "stick=(%d,%d) cam=(%.2f,%.2f) face=(%.2f,%.2f) poll=%u",
                unsigned(ctx), gv.air ? 1 : 0, gv.hover ? 1 : 0, out.dodgeKeysX, out.dodgeKeysY,
                gv.dodge.valid ? 1 : 0, out.dodgeAir ? "AIR MOVE request" : "evade macro ",
                out.dodgeAir ? "" : kKind[out.dodgeKind & 3], frame.lx, frame.ly, gv.dodge.camFwd[0],
                gv.dodge.camFwd[1], gv.dodge.face[0], gv.dodge.face[1], gv.poll);
  }
  return static_cast<uint16_t>(pad->buttons & ~before);
}

// lane E E5c: the cursor's title click = the game's own START (pc_controls.hpp)
bool pulseStart(int polls) {
  if (polls <= 0 || !enabled() || !pcactions::enabled()) return false;
  queueStartPulseForTest(polls, kStartPulseMaxMs);
  return true;
}

void cancelStartPulse() { g_startPulse.store(0, std::memory_order_relaxed); }

uint16_t takeStartPulse() {
  int n = g_startPulse.load(std::memory_order_relaxed);
  if (n > 0 && steadyMs() > g_startPulseUntil.load(std::memory_order_relaxed)) {   // past its deadline: dropped
    g_startPulse.store(0, std::memory_order_relaxed);
    return 0;
  }
  while (n > 0 && !g_startPulse.compare_exchange_weak(n, n - 1, std::memory_order_relaxed)) {
  }
  return n > 0 ? kXStart : 0;
}

void queueStartPulseForTest(int polls, int deadlineMs) {
  g_startPulseUntil.store(steadyMs() + deadlineMs, std::memory_order_relaxed);   // the deadline before the count
  g_startPulse.store(polls < 0 ? 0 : polls, std::memory_order_relaxed);
}

}  // namespace sr::kernel::pccontrols
