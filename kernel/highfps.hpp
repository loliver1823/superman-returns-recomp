// PORT FEATURE - high frame rate (render above the game's 30 Hz). docs/HIGH_FRAMERATE.md.
//
// The engine already separates simulation from rendering: FGGameSim ticks a FIXED 1/30 s
// step, FGGameRender runs its own GameKernelLoop at 30 Hz with a VARIABLE step, and the two
// meet in EA::StateStream (each sim tick publishes a time-stamped frame; the render tick
// reads the two frames that bracket its own clock and the renderables blend between them).
// The feature raises only the render loop's rate and lifts the platform's present caps
// (the emulated vblank and the host swap chain's sync interval). The simulation, its
// fixed step and every gameplay decision stay exactly as the console runs them.
//
// Gate: SR_HIGHFPS (default OFF while the prototype is measured):
//   unset / 0        off - the console's 30 Hz render loop and 60 Hz vblank, unchanged
//   1 / monitor      the host display refresh, vsync on
//   <N>              cap at N frames per second (e.g. 60, 120, 144); vsync on when N is at
//                    most the display refresh, off above it
//   uncapped / max   no cap, no vsync (tearing allowed where the display supports it)
// Overrides for experiments: SR_HIGHFPS_RENDER_HZ, SR_HIGHFPS_VBLANK_HZ, SR_HIGHFPS_SYNC (0/1).
// SR_HIGHFPS_PACE=0 silences the [HIGHFPS_PACE] frame-pacing lines (on whenever the
// feature is on; =1 also logs them with the feature off, for the 30 fps baseline).
#pragma once

#include <cstdint>
#include <string>

namespace sr::kernel::highfps {

enum class Mode : uint8_t { Off, Fixed, Monitor, Uncapped };

struct Config {
  Mode mode = Mode::Off;
  double targetHz = 30.0;    // what the player asked for (N, or the display refresh)
  double renderHz = 30.0;    // FGGameRender GameKernelLoop rate (the game's own value is 30)
  double vblankHz = 60.0;    // the emulated display controller's vblank rate (console: 60)
  int syncInterval = 1;      // host DXGI Present sync interval
  bool tearing = false;      // ask for DXGI_PRESENT_ALLOW_TEARING when syncInterval == 0
  bool paceLog = false;      // [HIGHFPS_PACE] lines
  double displayHz = 60.0;   // host display refresh at startup
};

// The rate the game itself gives FGGameRender (byte table 0x82911889/8D/91, all 30).
constexpr double kGameRenderHz = 30.0;
// Upper bound for the render loop and vblank rates ("uncapped").
constexpr double kMaxHz = 1000.0;

// Pure: resolve the config from the env strings (nullptr = unset) and the display rate.
Config configFromEnv(const char* mode, const char* renderHz, const char* vblankHz,
                     const char* sync, const char* pace, double displayHz) noexcept;

// Where the mode came from. Precedence: SR_HIGHFPS (env, any value incl. 0) > an automated
// roll (SR_INSTANCE_ID set without SR_PORT_FEATURES=1, or SR_PORT_FEATURES=0: off, so natural
// rolls never run a port feature) > the player's setting (<save root>/settings/display.ini, key
// target_fps = monitor | uncapped | <N> | off) > kPlayerDefault. iter-465: a PORT FEATURE roll
// (SR_INSTANCE_ID + SR_PORT_FEATURES=1) resolves like a player.
enum class Source : uint8_t { Env, AutomatedRoll, PortFeaturesOff, Player, Default };
const char* sourceName(Source s) noexcept;
// The player default (no setting yet). iter-465 polish: "monitor" = the display's refresh
// rate with vsync ON. The player lane's "120" tore on the owner's 60 Hz 4K TV (a 120 target
// above the refresh meant vsync off); the DISPLAY OPTIONS page (kernel/display_menu.cpp) lets
// the player pick 120 / UNLIMITED and vsync off (docs/HIGH_FRAMERATE.md section 8).
constexpr const char* kPlayerDefault = "monitor";
// Pure: pick the mode string. setting = the player's target_fps value (nullptr = none).
const char* resolveMode(const char* env, const char* instanceId, const char* portFeatures,
                        const char* setting, Source& src) noexcept;
// Pure: the target_fps value from the text of a display.ini ("" when absent). Lines are
// key=value, '#' starts a comment, whitespace around key and value is ignored.
bool parseTargetFps(const char* iniText, char* out, unsigned outSize) noexcept;

// The settings UI contract (the fe / mod-menu lane's FRAME RATE row): the stored value, and
// a store that validates (monitor | uncapped | off | 0 | <N> in 1..1000) and rewrites
// <save root>/settings/display.ini atomically, keeping every other line (the resolution
// lane's display keys) byte for byte. The render loop rate is set when FGGameRender is built
// at boot, so a new value takes effect at the next launch.
bool validTargetFps(const char* value) noexcept;
std::string withTargetFps(const std::string& iniText, const char* value);   // pure
// Pure: set `key = value` in an ini text, keeping every other line byte for byte (one line
// replaced per occurrence, or one appended).
std::string withIniKey(const std::string& iniText, const char* key, const char* value);
std::string playerTargetFps();
bool setPlayerTargetFps(const char* value) noexcept;

// iter-465 polish: VSYNC (display.ini `vsync = on | off`, default on). With the feature on and
// the source the player's (setting or default; an SR_HIGHFPS experiment keeps its own sync
// rule): vsync on = present sync interval 1 (never tears; a target above the display refresh
// is shown at the refresh), off = sync 0 with tearing allowed where the display supports it.
bool parseVsync(const char* iniText, bool& on) noexcept;   // pure; false = no vsync key
bool playerVsync();                                        // the stored value (default true)
bool setPlayerVsync(bool on) noexcept;
void applyVsync(Config& c, bool vsync) noexcept;           // pure

// iter-465 polish: LIVE changes from the settings menu (DISPLAY OPTIONS). Re-resolves the
// config from (target, vsync) and applies it at once: the host present sync / tearing, the
// emulated vblank rate (stub's vdVblankThread follows liveVblankHz) and the render loop
// rate, which a host worker sets through the game's own GameKernelMgr_SetTickRate_sub_82720418
// on the FGGameRender mgr (*0x8296D8DC) - the same setter the mgr's ctor uses at boot.
// Only for a run whose port features are on (a player, or SR_PORT_FEATURES=1); natural rolls
// never reach it (no menu). Returns false when refused (port features off, bad value).
bool applyLive(const char* targetFps, bool vsync) noexcept;
Config liveConfig() noexcept;               // the config in effect now (boot config until a change)
double liveVblankHz() noexcept;             // 0 = the console's 60 Hz path (feature off)
// The swap chain is created with ALLOW_TEARING whenever a live change could ask for tearing
// (port features on, or the boot config tears) and the factory supports it.
bool swapChainWantsTearingFlag() noexcept;

// Resolved once (thread-safe): the env, the automated-roll markers and the player setting.
const Config& config() noexcept;
Source configSource() noexcept;
inline bool active() noexcept { return config().mode != Mode::Off; }
const char* modeName(Mode m) noexcept;

// Host display refresh (primary display, EnumDisplaySettings); 60 when unknown.
double hostDisplayHz() noexcept;

// The vblank thread's period source: absolute QPC deadlines on a high-resolution waitable
// timer (no catch-up burst after a stall). Used only while the feature is on; off keeps
// the legacy 16667 us sleep_until cadence in stub's vdVblankThread.
class VblankClock {
 public:
  explicit VblankClock(double hz) noexcept;
  ~VblankClock();
  VblankClock(const VblankClock&) = delete;
  VblankClock& operator=(const VblankClock&) = delete;
  void wait() noexcept;

 private:
  int64_t periodQpc_ = 0, nextQpc_ = 0;
  void* timer_ = nullptr;
};

// Observer, called by stub_VdSwap on the presenting guest thread: frame-pacing statistics
// (present interval percentiles, the displayed StateStream time per present, render and
// sim tick rates from the engine's own GameKernelLoop counters). Reads guest memory only.
void onGuestSwap() noexcept;
// Observer, called by the host backend right after each swap-chain Present: the real display
// cadence (host present interval percentiles) for the same [HIGHFPS_PACE] line.
void onHostPresent() noexcept;

// Host swap chain policy (gfx): create the swap chain with ALLOW_TEARING when this is true
// (and the factory reports support); Present with this sync interval, adding
// DXGI_PRESENT_ALLOW_TEARING when the interval is 0 and the swap chain has the flag.
// iter-466 hfr DIAGNOSTIC SR_DIAG_SIM_FREEZE (default off): the sim loop is held periodically
// after a tick (diagAfterTick, from the PostTickStats hook); freezeDumpWanted() says whether the
// present dump should write this present (SR_PRESENT_DUMP_FREEZE=1: a few frames per freeze).
void diagAfterTick(uint32_t loopMgr) noexcept;
bool freezeDumpWanted(uint64_t presentNo) noexcept;
// iter-467 flicker: milliseconds the sim has been held by SR_DIAG_SIM_FREEZE (0 = not held).
double freezeHeldMs() noexcept;
int64_t renderLoopTimeNs() noexcept;   // render mgr +0x50 (ns), 0 before the loop exists (X2: gfx/present_luma.cpp)
// iter-467 native-a (save_gate_drift): the sim-held GUEST clock for the wall-clock shader time 0x29
// (kernel/timebase_obs.cpp holds it with the sim). simFreezeOn() = SR_DIAG_SIM_FREEZE is configured;
// freezeClock() returns, under one lock, the guest ticks (sr_guest_mftb, 50 MHz) of every COMPLETED hold
// and the start of the current one (0 = not held). Both stay 0 when the diagnostic is off.
bool simFreezeOn() noexcept;
struct FreezeClock {
  uint64_t heldTicks = 0;       // completed holds, guest ticks
  uint64_t holdStartTicks = 0;  // the current hold's start (sr_guest_mftb), 0 = not held
  uint32_t holdId = 0;          // the [SIM_FREEZE] #id of the current / last hold
};
FreezeClock freezeClock() noexcept;
// p2.zpd's bake (native/zpd-bake, Astra round 1 P2-2): at each edge of a hold - right after its [SIM_FREEZE] #id
// begin / end line - the occlusion queries' census (gfx/native_zpd logLine) with the why `freeze #<id> begin|end`:
// the counters at both edges bracket the hold's frozen, compared presents exactly (the periodic census, every 120
// presents, cannot place a query inside a hold's 40 dumped presents). Diagnostic only: a log line, no guest state;
// printed only while the queries are on (logLine's own rule). Called by the sim freeze; difftest calls it directly.
void freezeEdgeCensus(uint32_t id, bool begin) noexcept;
// iter-466 hfr: the refresh rate an EXCLUSIVE full-screen mode should ask for: 0 (the desktop's
// rate) unless the live frame-rate target is above desktopHz (then the target; UNLIMITED = 1000,
// i.e. the display's highest). SDL2's closest-mode rule (kept on SDL3: app/platform_sdl.hpp
// sdl3platform::closestModeSdl2) picks the lowest rate >= it.
int exclusiveRefreshHz(int desktopHz) noexcept;
bool wantTearing() noexcept;             // the LIVE config: present with ALLOW_TEARING at sync 0
uint32_t presentSyncInterval() noexcept;  // the LIVE config's sync interval

}  // namespace sr::kernel::highfps
