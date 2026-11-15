// PORT FEATURE - high frame rate. See kernel/highfps.hpp and docs/HIGH_FRAMERATE.md.
//
// Verified engine contracts this relies on (IDA, retail XEX; golden dump sound_gameplay):
//   - Four GameKernelLoop services, rates from the byte table 0x82911888.. (60/30/30/60 in
//     all three g_FeFlowConfig+420 modes): FSGameCore 0x8296D8D8 60 Hz fixed, FGGameRender
//     0x8296D8DC 30 Hz VARIABLE step (mode 1: dt = elapsed), FGGameSim 0x8296D8E0 30 Hz
//     FIXED step (33,333,334 ns), FGGameSound 0x8296D8E4 60 Hz variable.
//   - The render mgr is built by GameKernelMgrDerivedCtor_sub_82166178(mgr r3, ? r4,
//     rate f1), called only from StartService_FGGameRender_sub_82177F90. Its base ctor
//     GameKernelMgr_Ctor_sub_827209D8 -> GameKernelMgr_SetTickRate_sub_82720418 stores
//     +0x3C rate and +0x48 period = 1e9/rate + 0.5 ns.
//   - FSGameRenderMain_Tick_sub_821491A0 (the render tick): BeginRead(group0, render+0x50)
//     -> render+0x50 = group+0x30 read stamp -> renderer->vt[+0xC] Draw(dt +0x84) ->
//     EndRead. StateStreamGroup_BeginRead_sub_82787520 picks cur (stamp <= time) and next
//     (stamp > time), t = (time-cur)/(next-cur); the renderables read both blocks through
//     StateStreamGroup_GetReadBlocks_sub_827877A8 and blend (camera: two matrices nlerped
//     + a float lerped in sub_8277A048, snap when the camera id differs).
// This file changes ONE thing in the guest when the feature is on: the rate passed to the
// render mgr's ctor. Everything else here is observation or host-side presentation.

#include "highfps.hpp"
#include "ab_toggle.hpp"   // iter-467 guestcode: in-roll A/B mask per pace window

#include "guest/ppc/ppc_recomp_shared.h"
#include "guest_context.hpp"
#include "host_automation.hpp"   // iter-465 polish: live changes only with port features on
#include "xam_content.hpp"   // the save root that holds settings/display.ini
#include "sr_timebase.hpp"   // sr_guest_mftb: the held guest clock (shader time 0x29)
#include "../app/log.hpp"
#include "../gfx/native_zpd.hpp"   // freezeEdgeCensus: the occlusion queries' census at each hold's edges

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
#endif

extern "C" void __imp__sub_82166178(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_82720858(PPCContext& __restrict ctx, uint8_t* base);

namespace sr::kernel::highfps {

namespace {

bool parseDouble(const char* s, double& out) noexcept {
  if (!s || !*s) return false;
  char* end = nullptr;
  const double v = std::strtod(s, &end);
  if (end == s || !std::isfinite(v)) return false;
  out = v;
  return true;
}

bool ieq(const char* a, const char* b) noexcept {
  if (!a || !b) return false;
  for (; *a && *b; ++a, ++b) {
    char x = *a, y = *b;
    if (x >= 'A' && x <= 'Z') x = char(x - 'A' + 'a');
    if (y >= 'A' && y <= 'Z') y = char(y - 'A' + 'a');
    if (x != y) return false;
  }
  return *a == 0 && *b == 0;
}

double clampHz(double v) noexcept { return std::min(kMaxHz, std::max(1.0, v)); }

int64_t qpcNow() noexcept {
#if defined(_WIN32)
  LARGE_INTEGER c;
  QueryPerformanceCounter(&c);
  return c.QuadPart;
#else
  return std::chrono::steady_clock::now().time_since_epoch().count();
#endif
}

int64_t qpcFreq() noexcept {
#if defined(_WIN32)
  static const int64_t f = [] {
    LARGE_INTEGER q;
    QueryPerformanceFrequency(&q);
    return q.QuadPart;
  }();
  return f;
#else
  return 1000000000;
#endif
}

}  // namespace

const char* modeName(Mode m) noexcept {
  switch (m) {
    case Mode::Off: return "off";
    case Mode::Fixed: return "fixed";
    case Mode::Monitor: return "monitor";
    case Mode::Uncapped: return "uncapped";
  }
  return "?";
}

Config configFromEnv(const char* mode, const char* renderHz, const char* vblankHz,
                     const char* sync, const char* pace, double displayHz) noexcept {
  Config c;
  c.displayHz = (displayHz >= 23.0 && displayHz <= 1000.0) ? displayHz : 60.0;
  double n = 0.0;
  if (!mode || !*mode || ieq(mode, "0") || ieq(mode, "off")) {
    c.mode = Mode::Off;
  } else if (ieq(mode, "1") || ieq(mode, "monitor") || ieq(mode, "refresh") || ieq(mode, "vsync")) {
    c.mode = Mode::Monitor;
    c.targetHz = c.displayHz;
    c.renderHz = c.displayHz;
    c.vblankHz = clampHz(std::max(60.0, 2.0 * c.displayHz));
    c.syncInterval = 1;
  } else if (ieq(mode, "uncapped") || ieq(mode, "max") || ieq(mode, "unlimited")) {
    c.mode = Mode::Uncapped;
    c.targetHz = kMaxHz;
    c.renderHz = kMaxHz;
    c.vblankHz = kMaxHz;
    c.syncInterval = 0;
    c.tearing = true;
  } else if (parseDouble(mode, n) && n >= 1.0) {
    c.mode = Mode::Fixed;
    c.targetHz = clampHz(n);
    c.renderHz = c.targetHz;
    // The game presents with D3DPRESENT_INTERVAL_ONE (dev+0x33A4 = 1, golden): at least
    // one vblank between flips. Twice the render rate keeps that pacing from binding
    // below the cap even with millisecond jitter on the render loop's clock.
    c.vblankHz = clampHz(std::max(60.0, 2.0 * c.targetHz));
    c.syncInterval = (c.targetHz <= c.displayHz + 0.5) ? 1 : 0;
    c.tearing = c.syncInterval == 0;
  } else {
    c.mode = Mode::Off;   // unparsable: the console behaviour
  }
  if (c.mode != Mode::Off) {
    double v = 0.0;
    if (parseDouble(renderHz, v) && v >= 1.0) c.renderHz = clampHz(v);
    if (parseDouble(vblankHz, v) && v >= 1.0) c.vblankHz = clampHz(v);
    if (sync && (sync[0] == '0' || sync[0] == '1') && sync[1] == 0) {
      c.syncInterval = sync[0] - '0';
      c.tearing = c.syncInterval == 0;
    }
  }
  if (pace && pace[0] == '0' && pace[1] == 0) c.paceLog = false;
  else if (pace && pace[0] == '1' && pace[1] == 0) c.paceLog = true;
  else c.paceLog = c.mode != Mode::Off;
  return c;
}

double hostDisplayHz() noexcept {
#if defined(_WIN32)
  DEVMODEW dm{};
  dm.dmSize = sizeof(dm);
  if (EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &dm) && dm.dmDisplayFrequency > 1)
    return double(dm.dmDisplayFrequency);
#endif
  return 60.0;
}

const char* sourceName(Source s) noexcept {
  switch (s) {
    case Source::Env: return "env SR_HIGHFPS";
    case Source::AutomatedRoll: return "automated roll (SR_INSTANCE_ID): port features off";
    case Source::PortFeaturesOff: return "SR_PORT_FEATURES=0";
    case Source::Player: return "player setting display.ini target_fps";
    case Source::Default: return "player default";
  }
  return "?";
}

const char* resolveMode(const char* env, const char* instanceId, const char* portFeatures,
                        const char* setting, Source& src) noexcept {
  if (env) {   // explicit, any value (=0 is the A/B off switch)
    src = Source::Env;
    return env;
  }
  // iter-465 player: an automated roll is off UNLESS it is a PORT FEATURE roll
  // (SR_PORT_FEATURES=1, kernel/host_automation.hpp portFeaturesFor); then it resolves like a
  // player (its scratch save root has no display.ini, so the default applies).
  const bool pfOn = portFeatures && portFeatures[0] == '1' && portFeatures[1] == 0;
  if (instanceId && *instanceId && !pfOn) {
    src = Source::AutomatedRoll;
    return "0";
  }
  if (portFeatures && portFeatures[0] == '0' && portFeatures[1] == 0) {
    src = Source::PortFeaturesOff;
    return "0";
  }
  if (setting && *setting) {
    src = Source::Player;
    return setting;
  }
  src = Source::Default;
  return kPlayerDefault;
}

bool parseTargetFps(const char* iniText, char* out, unsigned outSize) noexcept {
  if (!out || outSize == 0) return false;
  out[0] = 0;
  if (!iniText) return false;
  std::istringstream in(iniText);
  std::string line;
  bool found = false;
  auto trim = [](std::string& v) {
    static const char kWs[] = {' ', '\t', '\r', '\n', 0};
    const size_t a = v.find_first_not_of(kWs);
    const size_t b = v.find_last_not_of(kWs);
    v = (a == std::string::npos) ? std::string() : v.substr(a, b - a + 1);
  };
  while (std::getline(in, line)) {
    const size_t hash = line.find('#');
    if (hash != std::string::npos) line.resize(hash);
    const size_t eq = line.find('=');
    if (eq == std::string::npos) continue;
    std::string key = line.substr(0, eq), val = line.substr(eq + 1);
    trim(key);
    trim(val);
    if (key != "target_fps" || val.empty() || val.size() + 1 > outSize) continue;
    std::memcpy(out, val.c_str(), val.size() + 1);
    found = true;   // the last one wins
  }
  return found;
}

namespace {
Source g_source = Source::Default;

// settings/display.ini is shared with the display settings (render/resscale: res_scale,
// display_mode, window/output size); this feature reads only its target_fps key.
std::string readPlayerTargetFps() {
  try {
    std::error_code ec;
    const std::filesystem::path p = sr::kernel::xamContentRoot() / "settings" / "display.ini";
    if (!std::filesystem::exists(p, ec)) return {};
    std::ifstream f(p, std::ios::binary);
    if (!f) return {};
    std::ostringstream ss;
    ss << f.rdbuf();
    char v[32];
    return parseTargetFps(ss.str().c_str(), v, sizeof v) ? std::string(v) : std::string();
  } catch (...) {
    return {};
  }
}
}  // namespace

bool validTargetFps(const char* v) noexcept {
  if (!v || !*v) return false;
  if (ieq(v, "monitor") || ieq(v, "uncapped") || ieq(v, "off") || ieq(v, "0")) return true;
  char* end = nullptr;
  const double n = std::strtod(v, &end);
  return end && end != v && *end == 0 && std::isfinite(n) && n >= 1.0 && n <= kMaxHz;
}

std::string withTargetFps(const std::string& iniText, const char* value) {
  return withIniKey(iniText, "target_fps", value);
}

std::string withIniKey(const std::string& iniText, const char* keyName, const char* value) {
  // Keep every other line (the resolution lane's display keys, comments) byte for byte;
  // replace each `keyName` line, or append one line when there is none.
  std::string out;
  out.reserve(iniText.size() + 32);
  bool replaced = false;
  const std::string eol = iniText.find("\r\n") != std::string::npos ? "\r\n" : "\n";
  size_t pos = 0;
  while (pos < iniText.size()) {
    const size_t nl = iniText.find('\n', pos);
    const size_t end = nl == std::string::npos ? iniText.size() : nl + 1;
    const std::string line = iniText.substr(pos, end - pos);
    std::string body = line;
    const size_t hash = body.find('#');
    if (hash != std::string::npos) body.resize(hash);
    const size_t eq = body.find('=');
    bool isKey = false;
    if (eq != std::string::npos) {
      std::string key = body.substr(0, eq);
      const size_t a = key.find_first_not_of(" \t");
      const size_t b = key.find_last_not_of(" \t");
      key = a == std::string::npos ? std::string() : key.substr(a, b - a + 1);
      isKey = key == keyName;
    }
    if (isKey) {
      out += keyName;
      out += "=";
      out += value;
      if (nl != std::string::npos)
        out += (line.size() >= 2 && line[line.size() - 2] == '\r') ? "\r\n" : "\n";
      replaced = true;
    } else {
      out += line;
    }
    pos = end;
  }
  if (!replaced) {
    if (!out.empty() && out.back() != '\n') out += eol;
    out += keyName;
    out += "=";
    out += value;
    out += eol;
  }
  return out;
}

std::string playerTargetFps() { return readPlayerTargetFps(); }

namespace {
// Atomic rewrite of settings/display.ini with one key changed (every other line kept).
bool writePlayerKey(const char* key, const char* value) noexcept {
  try {
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path dir = sr::kernel::xamContentRoot() / "settings";
    fs::create_directories(dir, ec);
    const fs::path p = dir / "display.ini";
    std::string text;
    if (fs::exists(p, ec)) {
      std::ifstream f(p, std::ios::binary);
      std::ostringstream ss;
      ss << f.rdbuf();
      text = ss.str();
    }
    const std::string next = withIniKey(text, key, value);
    const fs::path tmp = dir / "display.ini.tmp";
    {
      std::ofstream o(tmp, std::ios::binary | std::ios::trunc);
      if (!o) return false;
      o.write(next.data(), std::streamsize(next.size()));
      if (!o) return false;
    }
    fs::rename(tmp, p, ec);
    if (ec) {
      fs::remove(tmp, ec);
      return false;
    }
    SR_LOG_BOOT(sr::LogSeverity::Info, "[HIGHFPS] player %s = %s (settings/display.ini)", key, value);
    return true;
  } catch (...) {
    return false;
  }
}

std::string readPlayerDisplayIni() {
  try {
    std::error_code ec;
    const std::filesystem::path p = sr::kernel::xamContentRoot() / "settings" / "display.ini";
    if (!std::filesystem::exists(p, ec)) return {};
    std::ifstream f(p, std::ios::binary);
    if (!f) return {};
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
  } catch (...) {
    return {};
  }
}
}  // namespace

bool setPlayerTargetFps(const char* value) noexcept {
  if (!validTargetFps(value)) return false;
  return writePlayerKey("target_fps", value);
}

bool parseVsync(const char* iniText, bool& on) noexcept {
  if (!iniText) return false;
  std::istringstream in(iniText);
  std::string line;
  bool found = false;
  auto trim = [](std::string& v) {
    const size_t a = v.find_first_not_of(" \t\r\n");
    const size_t b = v.find_last_not_of(" \t\r\n");
    v = (a == std::string::npos) ? std::string() : v.substr(a, b - a + 1);
  };
  while (std::getline(in, line)) {
    const size_t hash = line.find('#');
    if (hash != std::string::npos) line.resize(hash);
    const size_t eq = line.find('=');
    if (eq == std::string::npos) continue;
    std::string key = line.substr(0, eq), val = line.substr(eq + 1);
    trim(key);
    trim(val);
    if (key != "vsync") continue;
    if (ieq(val.c_str(), "on") || val == "1" || ieq(val.c_str(), "true")) {
      on = true;
      found = true;
    } else if (ieq(val.c_str(), "off") || val == "0" || ieq(val.c_str(), "false")) {
      on = false;
      found = true;
    }
  }
  return found;
}

bool playerVsync() {
  bool on = true;
  const std::string text = readPlayerDisplayIni();
  if (!parseVsync(text.c_str(), on)) on = true;
  return on;
}

bool setPlayerVsync(bool on) noexcept { return writePlayerKey("vsync", on ? "on" : "off"); }

void applyVsync(Config& c, bool vsync) noexcept {
  if (c.mode == Mode::Off) return;   // the console's present path: sync 1, as before
  c.syncInterval = vsync ? 1 : 0;
  c.tearing = !vsync;
}

Source configSource() noexcept {
  (void)config();
  return g_source;
}

const Config& config() noexcept {
  static const Config c = [] {
    const char* env = std::getenv("SR_HIGHFPS");
    const char* inst = std::getenv("SR_INSTANCE_ID");
    const char* pf = std::getenv("SR_PORT_FEATURES");
    // The player's file is read only when nothing above it decides (an automated roll
    // never opens the player's settings).
    std::string setting;
    const bool pfOn = pf && pf[0] == '1' && pf[1] == 0;
    if (!env && !(inst && *inst && !pfOn) && !(pf && pf[0] == '0' && pf[1] == 0)) setting = readPlayerTargetFps();
    Source src = Source::Default;
    const char* mode = resolveMode(env, inst, pf, setting.empty() ? nullptr : setting.c_str(), src);
    g_source = src;
    Config r = configFromEnv(mode, std::getenv("SR_HIGHFPS_RENDER_HZ"),
                             std::getenv("SR_HIGHFPS_VBLANK_HZ"), std::getenv("SR_HIGHFPS_SYNC"),
                             std::getenv("SR_HIGHFPS_PACE"), hostDisplayHz());
    // iter-465 polish: the player's VSYNC (default on) decides the present sync for a
    // player-sourced mode; an SR_HIGHFPS / SR_HIGHFPS_SYNC experiment keeps its own rule.
    if ((src == Source::Player || src == Source::Default) && !std::getenv("SR_HIGHFPS_SYNC"))
      applyVsync(r, playerVsync());
    if (r.mode != Mode::Off)
      SR_LOG_BOOT(sr::LogSeverity::Info,
                  "[HIGHFPS] PORT FEATURE on (%s): mode=%s target=%.1f Hz render-loop=%.1f Hz (game: 30) "
                  "vblank=%.1f Hz (console: 60) present-sync=%d tearing=%d display=%.1f Hz; "
                  "the simulation stays on its fixed 1/30 s step",
                  sourceName(src), modeName(r.mode), r.targetHz, r.renderHz, r.vblankHz, r.syncInterval,
                  r.tearing ? 1 : 0, r.displayHz);
    else if (src != Source::Default || r.paceLog)
      SR_LOG_BOOT(sr::LogSeverity::Info, "[HIGHFPS] off (%s): the console's 30 Hz render loop and 60 Hz vblank",
                  sourceName(src));
    return r;
  }();
  return c;
}

// ---------------------------------------------------------------------------
// iter-465 polish: the LIVE config (the settings menu changes it; boot config until then).

namespace {
std::mutex g_liveMu;
Config g_live;
bool g_liveInit = false;
std::atomic<uint32_t> g_liveSync{1};
std::atomic<bool> g_liveTear{false};
std::atomic<double> g_liveVblank{0.0};
std::atomic<bool> g_livePace{false};

void publishLive(const Config& c) {
  g_live = c;
  g_liveInit = true;
  const bool on = c.mode != Mode::Off;
  g_liveSync.store(on ? uint32_t(c.syncInterval) : 1u, std::memory_order_relaxed);
  g_liveTear.store(on && c.tearing, std::memory_order_relaxed);
  g_liveVblank.store(on ? c.vblankHz : 0.0, std::memory_order_relaxed);
  g_livePace.store(c.paceLog, std::memory_order_relaxed);
}

void ensureLive() {
  static std::once_flag once;
  std::call_once(once, [] {
    const Config& boot = config();
    std::lock_guard<std::mutex> lk(g_liveMu);
    if (!g_liveInit) publishLive(boot);
  });
}

// The render loop rate: the game's own setter (GameKernelMgr_SetTickRate_sub_82720418: under the
// mgr CS, +0x3C rate and +0x48 period = 1e9/rate + 0.5 ns), called ON THE RENDER LOOP'S OWN
// THREAD right after its tick (the GameKernelLoop_PostTickStats_sub_82720858 hook below): no host
// thread touches the guest, and the mgr CS is taken by the thread that owns the loop.
std::atomic<double> g_rateWant{0.0};

void queueRenderRate(double hz) { g_rateWant.store(hz, std::memory_order_release); }
}  // namespace

Config liveConfig() noexcept {
  ensureLive();
  std::lock_guard<std::mutex> lk(g_liveMu);
  return g_live;
}

double liveVblankHz() noexcept {
  ensureLive();
  return g_liveVblank.load(std::memory_order_relaxed);
}

bool swapChainWantsTearingFlag() noexcept {
  return (active() && config().tearing) || sr::kernel::automation::portFeaturesOn();
}

bool applyLive(const char* targetFps, bool vsync) noexcept {
  if (!sr::kernel::automation::portFeaturesOn() || !validTargetFps(targetFps)) return false;
  ensureLive();
  const Config& boot = config();
  Config c = configFromEnv(targetFps, nullptr, nullptr, nullptr, boot.paceLog ? "1" : nullptr,
                           boot.displayHz);
  applyVsync(c, vsync);
  {
    std::lock_guard<std::mutex> lk(g_liveMu);
    publishLive(c);
  }
  const double renderHz = c.mode == Mode::Off ? kGameRenderHz : c.renderHz;
  queueRenderRate(renderHz);
  SR_LOG_BOOT(sr::LogSeverity::Info,
              "[HIGHFPS] live (settings menu): target_fps=%s vsync=%s -> mode=%s target=%.1f Hz "
              "render-loop=%.1f Hz vblank=%.1f Hz present-sync=%d tearing=%d display=%.1f Hz",
              targetFps, vsync ? "on" : "off", modeName(c.mode), c.targetHz, renderHz,
              c.mode == Mode::Off ? 60.0 : c.vblankHz, c.mode == Mode::Off ? 1 : c.syncInterval,
              c.mode != Mode::Off && c.tearing ? 1 : 0, c.displayHz);
  return true;
}

int exclusiveRefreshHz(int desktopHz) noexcept {
  const Config c = liveConfig();
  if (c.mode == Mode::Off) return 0;
  if (c.mode == Mode::Uncapped) return int(kMaxHz);
  if (desktopHz > 0 && c.targetHz <= double(desktopHz) + 0.5) return 0;
  return int(std::ceil(c.targetHz - 0.01));
}

bool wantTearing() noexcept {
  ensureLive();
  return g_liveTear.load(std::memory_order_relaxed);
}

uint32_t presentSyncInterval() noexcept {
  ensureLive();
  return g_liveSync.load(std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------
// Vblank period source (only while the feature is on).

VblankClock::VblankClock(double hz) noexcept {
  const double h = clampHz(hz);
  periodQpc_ = std::max<int64_t>(1, int64_t(double(qpcFreq()) / h + 0.5));
  nextQpc_ = qpcNow();
#if defined(_WIN32)
  timer_ = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                  TIMER_ALL_ACCESS);
#endif
}

VblankClock::~VblankClock() {
#if defined(_WIN32)
  if (timer_) CloseHandle(static_cast<HANDLE>(timer_));
#endif
}

void VblankClock::wait() noexcept {
  nextQpc_ += periodQpc_;
  const int64_t now = qpcNow();
  if (nextQpc_ <= now) {   // late (preempted): re-base, no catch-up burst of vblanks
    nextQpc_ = now;
    return;
  }
#if defined(_WIN32)
  if (timer_) {
    LARGE_INTEGER due;
    due.QuadPart = -((nextQpc_ - now) * 10000000 / qpcFreq());   // relative, 100 ns
    if (due.QuadPart == 0) due.QuadPart = -1;
    if (SetWaitableTimer(static_cast<HANDLE>(timer_), &due, 0, nullptr, nullptr, FALSE)) {
      WaitForSingleObject(static_cast<HANDLE>(timer_), INFINITE);
      return;
    }
  }
#endif
  std::this_thread::sleep_for(std::chrono::nanoseconds((nextQpc_ - now) * 1000000000 / qpcFreq()));
}

// ---------------------------------------------------------------------------
// Frame-pacing observer.

namespace {

constexpr uint32_t kRenderMgrPtr = 0x8296D8DCu;   // FGGameRender GameKernelLoop mgr
constexpr uint32_t kSimMgrPtr = 0x8296D8E0u;      // FGGameSim GameKernelLoop mgr
constexpr uint32_t kStateStreamMgrPtr = 0x829A93C8u;  // EA::StateStream::Manager*

bool plausible(uint32_t va) noexcept { return va >= 0x00010000u && va < 0xC0000000u; }

int64_t readI64(uint32_t va) noexcept {
  const uint64_t hi = sr::kernel::readGuestU32BE(va);
  const uint64_t lo = sr::kernel::readGuestU32BE(va + 4u);
  return int64_t((hi << 32) | lo);
}

float readF32(uint32_t va) noexcept {
  const uint32_t b = sr::kernel::readGuestU32BE(va);
  float f;
  std::memcpy(&f, &b, 4);
  return f;
}

struct Pace {
  std::mutex mu;
  int64_t windowStart = 0, lastSwap = 0;
  int64_t lastStamp = 0;
  bool haveStamp = false;
  uint64_t window = 0;
  uint32_t renderTicks0 = 0, simTicks0 = 0;
  int64_t simTime0 = 0;
  bool haveTicks0 = false;
  std::vector<float> intervalMs, stepMs;
  uint32_t holds = 0, backsteps = 0, tZero = 0, tCount = 0;
  double absErrSum = 0.0, tSum = 0.0;
  // Host side (D3D12Backend::presentSwap, the presenting host thread): the real display
  // cadence, which the guest-side VdSwap intervals only approximate.
  std::mutex hostMu;
  int64_t lastHost = 0;
  std::vector<float> hostIntervalMs;
};

Pace& pace() {
  static Pace p;
  return p;
}

float pct(std::vector<float>& v, double q) {
  if (v.empty()) return 0.f;
  const size_t k = std::min(v.size() - 1, size_t(q * double(v.size() - 1) + 0.5));
  std::nth_element(v.begin(), v.begin() + std::ptrdiff_t(k), v.end());
  return v[k];
}

}  // namespace

void onGuestSwap() noexcept {
  ensureLive();
  if (!g_livePace.load(std::memory_order_relaxed)) return;
  const int64_t now = qpcNow();
  const double qpcMs = 1000.0 / double(qpcFreq());

  // The displayed StateStream time: render mgr +0x50 holds the read stamp of the frame
  // being drawn (set right after BeginRead; PostTickStats adds dt only after Draw).
  const uint32_t rmgr = sr::kernel::readGuestU32BE(kRenderMgrPtr);
  const uint32_t smgr = sr::kernel::readGuestU32BE(kSimMgrPtr);
  const bool haveMgrs = plausible(rmgr) && plausible(smgr);
  int64_t stamp = 0, simTime = 0;
  uint32_t renderTicks = 0, simTicks = 0;
  float t = 0.f;
  bool haveT = false;
  if (haveMgrs) {
    stamp = readI64(rmgr + 0x50u);
    renderTicks = sr::kernel::readGuestU32BE(rmgr + 0x80u);
    simTime = readI64(smgr + 0x50u);
    simTicks = sr::kernel::readGuestU32BE(smgr + 0x80u);
    const uint32_t ssm = sr::kernel::readGuestU32BE(kStateStreamMgrPtr);
    const uint32_t arr = plausible(ssm) ? sr::kernel::readGuestU32BE(ssm) : 0u;
    const uint32_t g0 = plausible(arr) ? sr::kernel::readGuestU32BE(arr) : 0u;
    if (plausible(g0)) {
      t = readF32(g0 + 0x2Cu);
      haveT = std::isfinite(t);
    }
  }

  Pace& p = pace();
  std::lock_guard<std::mutex> lk(p.mu);
  if (!p.windowStart) p.windowStart = now;
  if (p.lastSwap) {
    const double dWall = double(now - p.lastSwap) * qpcMs;
    p.intervalMs.push_back(float(dWall));
    if (haveMgrs && p.haveStamp) {
      const double dShown = double(stamp - p.lastStamp) * 1e-6;
      p.stepMs.push_back(float(dShown));
      if (stamp == p.lastStamp) p.holds++;
      if (stamp < p.lastStamp) p.backsteps++;
      p.absErrSum += std::fabs(dShown - dWall);
    }
  }
  if (haveT) {
    p.tCount++;
    p.tSum += t;
    if (t == 0.f) p.tZero++;
  }
  p.lastSwap = now;
  if (haveMgrs) {
    p.lastStamp = stamp;
    p.haveStamp = true;
    if (!p.haveTicks0) {
      p.renderTicks0 = renderTicks;
      p.simTicks0 = simTicks;
      p.simTime0 = simTime;
      p.haveTicks0 = true;
    }
  }

  const double winMs = double(now - p.windowStart) * qpcMs;
  if (winMs < 2000.0) return;
  std::vector<float> host;
  {
    std::lock_guard<std::mutex> hk(p.hostMu);
    host.swap(p.hostIntervalMs);
  }
  const size_t nh = host.size();
  const float h50 = pct(host, 0.50), h95 = pct(host, 0.95), h99 = pct(host, 0.99), hmax = pct(host, 1.0);
  const size_t n = p.intervalMs.size();
  const float i50 = pct(p.intervalMs, 0.50), i95 = pct(p.intervalMs, 0.95),
              i99 = pct(p.intervalMs, 0.99), imax = pct(p.intervalMs, 1.0);
  const float s50 = pct(p.stepMs, 0.50), s95 = pct(p.stepMs, 0.95);
  const size_t ns = p.stepMs.size();
  // iter-466 hfr: the engine's own per-loop tick cost (GameKernelLoop_PostTickStats_sub_82720858:
  // +0xA0 = average tick duration in ns over its last 0.5 s window, on the 1 ms kernel clock).
  // The render loop's figure is the whole FGGameRender tick: guest render work, the XDK command
  // building and any GPU-ring / fence wait inside it.
  const double renderCostMs = haveMgrs ? double(readI64(rmgr + 0xA0u)) * 1e-6 : 0.0;
  const double simCostMs = haveMgrs ? double(readI64(smgr + 0xA0u)) * 1e-6 : 0.0;
  const double renderTps = p.haveTicks0 ? double(renderTicks - p.renderTicks0) * 1000.0 / winMs : 0.0;
  const double simTps = p.haveTicks0 ? double(simTicks - p.simTicks0) * 1000.0 / winMs : 0.0;
  const double simRate = p.haveTicks0 ? double(simTime - p.simTime0) * 1e-6 / winMs : 0.0;
  p.window++;
  // iter-467 guestcode (kernel/ab_toggle.hpp, SR_AB_TOGGLE): the toggled features' mask that held
  // during this window; the next window's mask takes effect now.
  char abTxt[24] = "";
  if (sr::abt::active())
    std::snprintf(abTxt, sizeof(abTxt), " | ab=0x%X", sr::abt::onWindowClose(p.window));
  SR_LOG_BOOT(sr::LogSeverity::Info,
              "[HIGHFPS_PACE] w=%llu %.2fs presents=%zu (%.1f/s) interval_ms p50=%.2f p95=%.2f "
              "p99=%.2f max=%.2f | shown_step_ms p50=%.2f p95=%.2f holds=%u back=%u "
              "|step-wall|=%.2fms | t0=%u/%u tmean=%.3f | render_ticks/s=%.1f sim_ticks/s=%.1f "
              "sim_time/wall=%.3f | host_presents=%zu (%.1f/s) host_ms p50=%.2f p95=%.2f p99=%.2f max=%.2f"
              " | tick_cost_ms render=%.2f sim=%.2f%s",
              (unsigned long long)p.window, winMs / 1000.0, n, double(n) * 1000.0 / winMs,
              i50, i95, i99, imax, s50, s95, p.holds, p.backsteps,
              ns ? p.absErrSum / double(ns) : 0.0, p.tZero, p.tCount,
              p.tCount ? p.tSum / double(p.tCount) : 0.0, renderTps, simTps, simRate,
              nh, double(nh) * 1000.0 / winMs, h50, h95, h99, hmax, renderCostMs, simCostMs, abTxt);
  p.intervalMs.clear();
  p.stepMs.clear();
  p.holds = p.backsteps = p.tZero = p.tCount = 0;
  p.absErrSum = p.tSum = 0.0;
  p.windowStart = now;
  if (haveMgrs) {
    p.renderTicks0 = renderTicks;
    p.simTicks0 = simTicks;
    p.simTime0 = simTime;
  }
}

void onHostPresent() noexcept {
  ensureLive();
  if (!g_livePace.load(std::memory_order_relaxed)) return;
  const int64_t now = qpcNow();
  Pace& p = pace();
  std::lock_guard<std::mutex> hk(p.hostMu);
  if (p.lastHost) p.hostIntervalMs.push_back(float(double(now - p.lastHost) * 1000.0 / double(qpcFreq())));
  p.lastHost = now;
  if (p.hostIntervalMs.size() > 100000) p.hostIntervalMs.clear();   // no guest swaps for a long time
}

// ---------------------------------------------------------------------------
// iter-466 hfr DIAGNOSTIC (default OFF, never in a player's run): SR_DIAG_SIM_FREEZE.
// Holds the FGGameSim loop's thread for <hold_ms> after one of its ticks, every <period_s> of
// wall time (first at <start_s>), while the render loop keeps drawing. The sim publishes nothing
// while held, so the render clock reaches the newest StateStream frame and stays there (t = 0,
// read stamp constant): every system on the SIM clock - and every render-side reader of the
// shown game time - must then draw the same picture every pass. Whatever still moves in the
// frames of a freeze runs on another clock (render passes, wall clock, media). With
// SR_PRESENT_DUMP=1 + SR_PRESENT_DUMP_FREEZE=1 the present dump writes a few frames of each
// freeze (freezeDumpWanted). It changes guest TIMING only (the sim thread sleeps after its tick;
// the debt clamp of GameKernelLoop_TimeGate drops the held time, as after any long tick); it
// writes no guest state. Not a port feature: a named opt-in diagnostic for the clock inventory
// (docs/HIGH_FRAMERATE.md section 6).
namespace {
struct FreezeCfg {
  bool on = false;
  double periodS = 20.0, holdMs = 3000.0, startS = 30.0;
};
const FreezeCfg& freezeCfg() {
  static const FreezeCfg c = [] {
    FreezeCfg r;
    const char* e = std::getenv("SR_DIAG_SIM_FREEZE");
    if (!e || !*e || e[0] == '0') return r;
    // <period_s>:<hold_ms>:<start_s> (':' or ',' - perf_run.py --gates splits on commas)
    std::string spec(e);
    std::replace(spec.begin(), spec.end(), ':', ',');
    double a = 0, b = 0, s = -1;
    const int n = std::sscanf(spec.c_str(), "%lf,%lf,%lf", &a, &b, &s);
    if (n >= 1 && a > 0) r.periodS = a;
    if (n >= 2 && b > 0) r.holdMs = b;
    if (n >= 3 && s >= 0) r.startS = s;
    r.on = true;
    SR_LOG_BOOT(sr::LogSeverity::Info,
                "[SIM_FREEZE] DIAGNOSTIC on: hold the sim loop %.0f ms every %.1f s from %.1f s",
                r.holdMs, r.periodS, r.startS);
    return r;
  }();
  return c;
}
std::atomic<uint32_t> g_freezeId{0};
std::atomic<int64_t> g_freezeStartQpc{0};   // 0 = the sim is not held
// iter-467 native-a: the held guest clock (freezeClock; the shader time 0x29 hold in timebase_obs.cpp).
// One lock: a reader must never see a hold's start together with that same hold already counted.
std::mutex g_clockMu;
uint64_t g_heldTicks = 0, g_holdStartTicks = 0;
uint32_t g_clockHoldId = 0;
std::atomic<uint32_t> g_freezeK{0}, g_freezeDumps{0};

void simFreezeAfterTick(uint32_t mgr) {
  const FreezeCfg& c = freezeCfg();
  if (!c.on) return;
  if (mgr != sr::kernel::readGuestU32BE(kSimMgrPtr)) return;
  static int64_t s_t0 = 0, s_next = 0;   // the sim loop's thread only
  const int64_t now = qpcNow();
  const double f = double(qpcFreq());
  if (!s_t0) {
    s_t0 = now;
    s_next = now + int64_t(c.startS * f);
  }
  if (now < s_next) return;
  const uint32_t id = g_freezeId.fetch_add(1) + 1;
  const uint32_t rmgr = sr::kernel::readGuestU32BE(kRenderMgrPtr);
  const uint32_t r0 = plausible(rmgr) ? sr::kernel::readGuestU32BE(rmgr + 0x80u) : 0u;
  g_freezeK.store(0);
  g_freezeDumps.store(0);
  {
    std::lock_guard<std::mutex> lk(g_clockMu);
    g_holdStartTicks = sr_guest_mftb();
    if (!g_holdStartTicks) g_holdStartTicks = 1;  // 0 means "not held"
    g_clockHoldId = id;
  }
  g_freezeStartQpc.store(now);
  SR_LOG_BOOT(sr::LogSeverity::Info,
              "[SIM_FREEZE] #%u begin at %.1f s: sim time %.4f s, sim ticks %u, render ticks %u", id,
              double(now - s_t0) / f, double(readI64(mgr + 0x50u)) * 1e-9,
              sr::kernel::readGuestU32BE(mgr + 0x80u), r0);
  sr::kernel::highfps::freezeEdgeCensus(id, true);
  const int64_t until = now + int64_t(c.holdMs * f / 1000.0);
  while (qpcNow() < until) ::Sleep(1);
  {
    std::lock_guard<std::mutex> lk(g_clockMu);
    const uint64_t t = sr_guest_mftb();
    if (t > g_holdStartTicks) g_heldTicks += t - g_holdStartTicks;
    g_holdStartTicks = 0;
  }
  g_freezeStartQpc.store(0);
  const uint32_t r1 = plausible(rmgr) ? sr::kernel::readGuestU32BE(rmgr + 0x80u) : 0u;
  SR_LOG_BOOT(sr::LogSeverity::Info,
              "[SIM_FREEZE] #%u end: %u render ticks while held (%.1f/s), shown stamp %.4f s", id,
              r1 - r0, double(r1 - r0) * 1000.0 / c.holdMs,
              plausible(rmgr) ? double(readI64(rmgr + 0x50u)) * 1e-9 : 0.0);
  sr::kernel::highfps::freezeEdgeCensus(id, false);
  s_next = qpcNow() + int64_t(c.periodS * f);
}
}  // namespace

void freezeEdgeCensus(uint32_t id, bool begin) noexcept {
  char why[40];
  std::snprintf(why, sizeof why, "freeze #%u %s", id, begin ? "begin" : "end");
  sr::gfx::zpd::logLine(why);
}

bool freezeDumpWanted(uint64_t presentNo) noexcept {
  // SR_PRESENT_DUMP_FREEZE=1: presents 0-2 and every 10th from 400 ms into each freeze, up to 10.
  // =2 (iter-467 flicker gate): EVERY present from 400 ms in, up to SR_PRESENT_DUMP_FREEZE_CAP
  // (default 40) per freeze - consecutive frozen passes, which must be pixel-identical.
  static const int s_mode = [] {
    const char* e = std::getenv("SR_PRESENT_DUMP_FREEZE");
    if (!e || !freezeCfg().on) return 0;
    return e[0] == '1' ? 1 : e[0] == '2' ? 2 : 0;
  }();
  static const uint32_t s_cap = [] {
    const char* e = std::getenv("SR_PRESENT_DUMP_FREEZE_CAP");
    const long v = e ? std::atol(e) : 0;
    return v > 0 ? uint32_t(v) : (s_mode == 2 ? 40u : 10u);
  }();
  if (!s_mode) return false;
  const int64_t s = g_freezeStartQpc.load(std::memory_order_acquire);
  if (!s) return false;
  const double ms = double(qpcNow() - s) * 1000.0 / double(qpcFreq());
  if (ms < 400.0) return false;   // the render clock is still catching up with the newest frame
  const uint32_t k = g_freezeK.fetch_add(1);
  if (s_mode == 1 && !(k < 3 || k % 10 == 0)) return false;
  if (g_freezeDumps.fetch_add(1) >= s_cap) return false;
  const uint32_t rmgr = sr::kernel::readGuestU32BE(kRenderMgrPtr);
  // Astra round 2 on native/zpd-bake (P2-1): the presented frame's own occlusion-query events (gfx/native_zpd
  // FrameStamp: taken at its swap packet in stream order, carried to this present) - `zpd=-` when this present has none
  char zpd[80] = " zpd=-";
  const sr::gfx::zpd::FrameStamp st = sr::gfx::zpd::presentingStamp();
  if (st.frame)
    std::snprintf(zpd, sizeof zpd, " zpd=%u rec=%u pm4=%u fswap=%u", st.begins, st.rec, st.pm4, st.frame);
  SR_LOG_BOOT(sr::LogSeverity::Info,
              "[SIM_FREEZE_DUMP] freeze=%u k=%u present=%llu held_ms=%.0f shown_stamp=%.4f s%s",
              g_freezeId.load(), k, (unsigned long long)presentNo, ms,
              plausible(rmgr) ? double(readI64(rmgr + 0x50u)) * 1e-9 : 0.0, zpd);
  return true;
}

bool simFreezeOn() noexcept { return freezeCfg().on; }

FreezeClock freezeClock() noexcept {
  FreezeClock c;
  if (!freezeCfg().on) return c;
  std::lock_guard<std::mutex> lk(g_clockMu);
  c.heldTicks = g_heldTicks;
  c.holdStartTicks = g_holdStartTicks;
  c.holdId = g_clockHoldId;
  return c;
}

double freezeHeldMs() noexcept {
  const int64_t s = g_freezeStartQpc.load(std::memory_order_acquire);
  if (!s) return 0.0;
  return double(qpcNow() - s) * 1000.0 / double(qpcFreq());
}

// Step 4 X2 (lane D, 2026-10-18; observe-only): the render loop's time, render mgr +0x50 (i64 ns; HIGH_FRAMERATE 2.1/2.2:
// render+0x50 = the StateStream group time the render tick reads). gfx/present_luma.cpp records it per present so a
// SR_DIAG_SIM_FREEZE hold's analysis can start once the drawn scene has stopped changing. 0 before the loop exists.
int64_t renderLoopTimeNs() noexcept {
  const uint32_t rmgr = sr::kernel::readGuestU32BE(kRenderMgrPtr);
  return plausible(rmgr) ? readI64(rmgr + 0x50u) : 0;
}

void diagAfterTick(uint32_t mgr) noexcept { simFreezeAfterTick(mgr); }

}  // namespace sr::kernel::highfps

// ---------------------------------------------------------------------------
// The one guest-visible change: FGGameRender's loop rate. Gate off = exact pass-through.

namespace {
// PORT FEATURE gate: SR_HIGHFPS (env) or the player's target_fps; never in an automated
// roll (SR_INSTANCE_ID / SR_PORT_FEATURES=0) unless SR_HIGHFPS names it.
bool renderRateOverrideOn() {
  // (gen_override_manifest.py reads a helper's FIRST inner block: keep the getenv in one.)
  static const bool on = [] {
    (void)std::getenv("SR_HIGHFPS");   // the gate's env name; config() resolves env + setting
    return sr::kernel::highfps::active();
  }();
  return on;
}
}  // namespace

// GameKernelMgrDerivedCtor_sub_82166178(mgr r3, r4, rate f1): the FGGameRender service mgr
// (only caller StartService_FGGameRender_sub_82177F90; the game passes 30.0 from the byte
// table). The ctor keeps mode 1 (variable step), so the render tick's dt stays the real
// elapsed time and the StateStream blend factor follows it.
PPC_FUNC(sub_82166178) {
  if (!renderRateOverrideOn()) {
    __imp__sub_82166178(ctx, base);
    return;
  }
  const double gameHz = ctx.f1.f64;
  ctx.f1.f64 = sr::kernel::highfps::config().renderHz;
  SR_LOG_BOOT(sr::LogSeverity::Info,
              "[HIGHFPS] FGGameRender loop rate %.1f -> %.1f Hz (mgr=0x%08X); FGGameSim keeps its "
              "fixed 30 Hz step",
              gameHz, ctx.f1.f64, ctx.r3.u32);
  __imp__sub_82166178(ctx, base);
}

// iter-465 polish: GameKernelLoop_PostTickStats_sub_82720858(loopMgr r3) runs on every
// GameKernelLoop's own thread after each tick. Exact pass-through unless the DISPLAY OPTIONS page
// changed the frame rate (highfps::applyLive): then, once, on the FGGameRender loop's thread
// (r3 == *0x8296D8DC), the game's own GameKernelMgr_SetTickRate_sub_82720418 sets the new rate.
// The next TimeGate of that loop uses the new period. Registers are restored to the original's
// return state, so the caller sees exactly what PostTickStats returned.
namespace {
// PORT FEATURE gate of the live render-rate hook: only a run whose port features are on can have
// the DISPLAY OPTIONS page (a player, or SR_PORT_FEATURES=1); natural rolls never take the branch.
// (gen_override_manifest.py reads a helper's FIRST inner block: keep the getenv in one.)
bool liveRateHookOn() {
  static const bool on = [] {
    (void)std::getenv("SR_PORT_FEATURES");   // the gate's env name; portFeaturesOn() resolves it
    return sr::kernel::automation::portFeaturesOn();
  }();
  return on;
}
}  // namespace

PPC_FUNC(sub_82720858) {
  const uint32_t mgr = ctx.r3.u32;
  __imp__sub_82720858(ctx, base);
  sr::kernel::highfps::diagAfterTick(mgr);   // SR_DIAG_SIM_FREEZE (diagnostic, default off)
  if (!liveRateHookOn()) return;
  double want = sr::kernel::highfps::g_rateWant.load(std::memory_order_acquire);
  if (want <= 0.0) return;
  if (mgr != sr::kernel::readGuestU32BE(0x8296D8DCu)) return;
  if (!sr::kernel::highfps::g_rateWant.compare_exchange_strong(want, 0.0)) return;
  const PPCContext after = ctx;
  ctx.r3.u64 = mgr;
  ctx.f1.f64 = want;
  sub_82720418(ctx, base);   // GameKernelMgr_SetTickRate
  ctx = after;
  const uint32_t rb = sr::kernel::readGuestU32BE(mgr + 0x3Cu);
  float rate;
  std::memcpy(&rate, &rb, 4);
  const uint64_t period = (uint64_t(sr::kernel::readGuestU32BE(mgr + 0x48u)) << 32) |
                          sr::kernel::readGuestU32BE(mgr + 0x4Cu);
  SR_LOG_BOOT(sr::LogSeverity::Info,
              "[HIGHFPS] live render-loop rate -> %.1f Hz via GameKernelMgr_SetTickRate on the render "
              "loop's thread (mgr=0x%08X): +0x3C=%.1f +0x48=%llu ns",
              want, mgr, double(rate), (unsigned long long)period);
}
