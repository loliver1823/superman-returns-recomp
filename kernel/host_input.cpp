// iter-464 input lane: the PC controller layer - contract in kernel/host_input.hpp,
// design + evidence in docs/INPUT_GLYPHS.md.
//
// References: reference/UnleashedRecomp/UnleashedRecomp/hid/driver/sdl_hid.cpp (the hints,
// the SDL->XInput button/axis translation, player 1 = last used controller) and
// reference/xenia-canary/src/xenia/hid/sdl/sdl_input_driver.cc (axis conversion: Y = ~v,
// triggers v >> 7; rumble through SDL_RumbleGamepad).
#include "host_input.hpp"
#include "sr_oracle.hpp"

#include "host_automation.hpp"   // iter-464 bgruns: the automation / input-isolation policy
#include "vpad_filter.hpp"       // iter-466: SR_HARNESS_VPAD, the ViGEm-only pad filter
#include "xam_content.hpp"
#include "../app/log.hpp"
#include "guest_context.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <sstream>
#include <system_error>
#include <vector>

namespace sr::kernel::hostinput {

namespace {

constexpr uint16_t kBtnBits[kControlCount] = {
    0x0001, 0x0002, 0x0004, 0x0008,  // DPAD up/down/left/right
    0x0010, 0x0020,                  // START, BACK
    0x0040, 0x0080,                  // L3, R3
    0x0100, 0x0200,                  // LB, RB
    0x1000, 0x2000, 0x4000, 0x8000,  // A, B, X, Y
    0, 0,                            // LT, RT (analog)
    0, 0, 0, 0, 0, 0, 0, 0,          // sticks (analog)
};

constexpr const char* kControlNames[kControlCount] = {
    "DpadUp", "DpadDown", "DpadLeft", "DpadRight", "Start", "Back", "LThumb", "RThumb",
    "LB", "RB", "A", "B", "X", "Y", "LT", "RT",
    "LStickUp", "LStickDown", "LStickLeft", "LStickRight",
    "RStickUp", "RStickDown", "RStickLeft", "RStickRight",
};

// Activation thresholds: a stick or trigger pushed past half travel makes its pad player 1.
// Higher than XInput's deadzones (7849 / 8689 / 30) on purpose: a slightly drifting idle pad
// must not steal player 1 from the device in the player's hands.
constexpr int kStickActivate = 16384;
constexpr int kTriggerActivate = 16384;  // SDL trigger axis is 0..32767

// SDL3: an SDL_JoystickID is an unsigned instance id and 0 is "no device" (SDL2: signed, -1).
constexpr SDL_JoystickID kNoPad = 0;

struct Pad {
  SDL_Gamepad* gc = nullptr;
  SDL_JoystickID id = kNoPad;
  Device kind = Device::Generic;
  std::string name;
};

struct State {
  std::mutex mu;                 // guards pads + active* (guest threads read pads)
  std::vector<Pad> pads;
  SDL_JoystickID activeId = kNoPad;  // player 1's pad (kNoPad = none)
  Device active = Device::None;  // the device used last
  bool p1Lost = false;           // player 1's active pad was unplugged, nothing used since
  bool inited = false;
  bool sdlPad = true;
  bool realInput = true;
  // iter-466 tooling: an automated run with SR_HARNESS_VPAD=1 opens ONLY ViGEm virtual pads
  // (kernel/vpad_filter.hpp); every other controller, the owner's included, is never opened.
  bool vpadOnly = false;
  bool glyphs = true;
  bool glyphPinned = false;
  GlyphSet glyphPin = GlyphSet::Xbox360;
  uint32_t glyphCycleMs = 0;  // SR_GLYPHS=cycle (test instrument): rotate every set
  std::atomic<KeyPromptSource> promptSrc{nullptr};  // the controls lane's, else the default
  float mouseAccum = 0.0f;   // SDL3: motion is float (whole pixels on Windows)
  // Rumble: guest threads publish, the main thread applies.
  std::atomic<uint32_t> vib{0};     // (left << 16) | right
  uint32_t vibApplied = 0;
  SDL_JoystickID vibPad = kNoPad;
  uint32_t vibLastMs = 0;
  std::atomic<uint32_t> gen{1};
  std::atomic<uint8_t> activeAtomic{uint8_t(Device::None)};
  std::atomic<bool> p1LostAtomic{false};
  uint32_t changeLogs = 0;
};

State& S() {
  static State s;
  return s;
}

bool envOn(const char* name, bool dflt) {
  const char* e = std::getenv(name);
  if (!e || !*e) return dflt;
  return e[0] != '0';
}

// The automation policy (kernel/host_automation.hpp, the bgruns lane): ONE rule for who is an
// automated run and whether it may see real input. Recomputed from the environment here (the
// pure computePolicy, not the cached policy()) so difftest can switch it between cases; the
// game reads the environment once at init, so both agree.
automation::Policy envPolicy() {
  auto env = [](const char* n) -> const char* {
    const char* v = std::getenv(n);
    return v && *v ? v : nullptr;
  };
#if SR_ORACLE
  const bool traceReplay = env("SR_GPU_TRACE_REPLAY") || env("SR_HLE_TRACE_REPLAY");
#else
  constexpr bool traceReplay = false;  // SR_ORACLE=0: no replay mode (app/main.cpp); the replay variables read as unset
#endif
  return automation::computePolicy(env("SR_INSTANCE_ID"), traceReplay, env("SR_WINDOW_BACKGROUND"),
                                   env("SR_HARNESS_INPUT"), env("SR_HARNESS_REAL_INPUT"));
}

bool automatedRun() { return envPolicy().automated; }

Device kindFromSdl(SDL_Gamepad* gc) {
  switch (SDL_GetGamepadType(gc)) {
    case SDL_GAMEPAD_TYPE_XBOX360:
    case SDL_GAMEPAD_TYPE_XBOXONE:
      return Device::Xbox;
    case SDL_GAMEPAD_TYPE_PS3:
    case SDL_GAMEPAD_TYPE_PS4:
      return Device::PS4;
    case SDL_GAMEPAD_TYPE_PS5:
      return Device::PS5;
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO:
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_LEFT:
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT:
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR:
      return Device::Switch;
    default:
      return Device::Generic;
  }
}

void publish(State& s) {  // caller holds s.mu
  s.activeAtomic.store(uint8_t(s.active), std::memory_order_relaxed);
  s.p1LostAtomic.store(s.p1Lost, std::memory_order_relaxed);
  s.gen.fetch_add(1, std::memory_order_release);
}

const Pad* findPad(const State& s, SDL_JoystickID id) {
  for (const auto& p : s.pads)
    if (p.id == id) return &p;
  return nullptr;
}

// caller holds s.mu
void setActive(State& s, Device d, SDL_JoystickID padId, const char* why) {
  const bool same = s.active == d && s.activeId == padId && !s.p1Lost;
  if (same) return;
  const Device prev = s.active;
  s.active = d;
  s.activeId = padId;
  s.p1Lost = false;
  publish(s);
  const uint32_t n = ++s.changeLogs;
  if (n <= 64 || n % 100 == 0) {
    const Pad* p = padId != kNoPad ? findPad(s, padId) : nullptr;
    SR_LOG_BOOT(sr::LogSeverity::Info,
                "[INPUT] player 1 = %s%s%s%s (was %s; %s) glyphs=%s #%u", deviceName(d),
                p ? " '" : "", p ? p->name.c_str() : "", p ? "'" : "", deviceName(prev), why,
                s.glyphs ? glyphSetName(s.glyphPinned ? s.glyphPin : glyphSetForDevice(d)) : "off",
                n);
  }
}

std::string trim(const std::string& s) {
  size_t a = 0, b = s.size();
  while (a < b && std::isspace(uint8_t(s[a]))) ++a;
  while (b > a && std::isspace(uint8_t(s[b - 1]))) --b;
  return s.substr(a, b - a);
}

bool ieq(const std::string& a, const char* b) {
  const size_t n = std::strlen(b);
  if (a.size() != n) return false;
  for (size_t i = 0; i < n; ++i)
    if (std::tolower(uint8_t(a[i])) != std::tolower(uint8_t(b[i]))) return false;
  return true;
}

// The player's glyph pin from <content root>/settings/input.ini (read-only: the file and its
// defaults belong to the controls lane, docs/PC_CONTROLS.md section 7).
void loadPlayerGlyphPin(State& s) {
  namespace fs = std::filesystem;
  const fs::path path = xamContentRoot() / "settings" / "input.ini";
  std::ifstream in(path, std::ios::binary);
  if (!in) return;  // no file yet: glyphs = auto
  std::ostringstream ss;
  ss << in.rdbuf();
  std::string errors;
  bool isAuto = true;
  GlyphSet pin = GlyphSet::Xbox360;
  if (readGlyphPin(ss.str(), &pin, &isAuto, &errors)) {
    s.glyphPinned = !isAuto;
    s.glyphPin = pin;
  }
  SR_LOG_BOOT(sr::LogSeverity::Info, "[INPUT] prompts from %s: glyphs=%s%s%s",
              path.string().c_str(), s.glyphPinned ? glyphSetName(s.glyphPin) : "auto",
              errors.empty() ? "" : "; problems: ", errors.c_str());
}

void readEnv(State& s) {
  s.sdlPad = envOn("SR_SDL_PAD", true);
  // Real pads and real keyboard/mouse device tracking follow the automation policy's
  // realInput (automated runs: off unless SR_HARNESS_REAL_INPUT=1).
  s.realInput = envPolicy().realInput;
  s.vpadOnly = !s.realInput && envPolicy().automated && envOn("SR_HARNESS_VPAD", false);
  // Glyphs: players always; automated runs only when SR_GLYPHS asks (the game's own icons
  // keep a roll comparable with the Xenia oracle) or the roll is a PORT FEATURE roll
  // (iter-465: SR_PORT_FEATURES=1, kernel/host_automation.hpp portFeaturesFor).
  {
    const char* inst = std::getenv("SR_INSTANCE_ID");
    const char* pf = std::getenv("SR_PORT_FEATURES");
    s.glyphs = automation::portFeaturesFor(inst && *inst ? inst : nullptr, pf && *pf ? pf : nullptr);
  }
  s.glyphPinned = false;
  s.glyphCycleMs = 0;
  if (const char* g = std::getenv("SR_GLYPHS"); g && *g) {
    if (g[0] == '0') {
      s.glyphs = false;
    } else if (std::strcmp(g, "cycle") == 0) {
      // TEST INSTRUMENT (renderer-only): every glyph set in turn on the same screens, so one
      // roll shows them all. SR_GLYPHS_CYCLE_MS sets the period (default 5000).
      s.glyphs = true;
      const char* ms = std::getenv("SR_GLYPHS_CYCLE_MS");
      s.glyphCycleMs = ms && std::atoi(ms) > 100 ? uint32_t(std::atoi(ms)) : 5000u;
    } else {
      bool isAuto = false;
      GlyphSet set{};
      if (parseGlyphSet(g, &set, &isAuto)) {
        s.glyphs = true;
        s.glyphPinned = !isAuto;
        s.glyphPin = set;
      }
    }
  }
}

}  // namespace

// ── names ─────────────────────────────────────────────────────────────────────
const char* deviceName(Device d) {
  switch (d) {
    case Device::None: return "none";
    case Device::Keyboard: return "keyboard";
    case Device::Xbox: return "xbox";
    case Device::PS4: return "ps4";
    case Device::PS5: return "ps5";
    case Device::Switch: return "switch";
    case Device::Generic: return "generic";
  }
  return "?";
}

const char* glyphSetName(GlyphSet g) {
  switch (g) {
    case GlyphSet::Xbox360: return "xbox";
    case GlyphSet::PS4: return "ps4";
    case GlyphSet::PS5: return "ps5";
    case GlyphSet::Switch: return "switch";
    case GlyphSet::Keyboard: return "keyboard";
    default: return "?";
  }
}

GlyphSet glyphSetForDevice(Device d) {
  switch (d) {
    case Device::Keyboard: return GlyphSet::Keyboard;
    case Device::PS4: return GlyphSet::PS4;
    case Device::PS5: return GlyphSet::PS5;
    case Device::Switch: return GlyphSet::Switch;
    default: return GlyphSet::Xbox360;
  }
}

bool parseGlyphSet(const char* str, GlyphSet* out, bool* isAuto) {
  const std::string s = trim(str ? str : "");
  *isAuto = false;
  if (ieq(s, "auto") || ieq(s, "1")) { *isAuto = true; return true; }
  if (ieq(s, "xbox") || ieq(s, "xbox360") || ieq(s, "x360")) { *out = GlyphSet::Xbox360; return true; }
  if (ieq(s, "ps4") || ieq(s, "playstation4") || ieq(s, "playstation")) { *out = GlyphSet::PS4; return true; }
  if (ieq(s, "ps5") || ieq(s, "playstation5")) { *out = GlyphSet::PS5; return true; }
  if (ieq(s, "switch") || ieq(s, "nintendo")) { *out = GlyphSet::Switch; return true; }
  if (ieq(s, "keyboard") || ieq(s, "kbm")) { *out = GlyphSet::Keyboard; return true; }
  return false;
}

const char* controlName(Control c) { return c < kControlCount ? kControlNames[c] : "?"; }
uint16_t controlButtonBit(Control c) { return c < kControlCount ? kBtnBits[c] : 0; }

// ── keyboard prompts ──────────────────────────────────────────────────────────
void setKeyPromptSource(KeyPromptSource fn) {
  State& s = S();
  s.promptSrc.store(fn, std::memory_order_release);
  s.gen.fetch_add(1, std::memory_order_release);
}

std::string promptInputFor(Control c) {
  if (c >= kControlCount) return {};
  if (const KeyPromptSource fn = S().promptSrc.load(std::memory_order_acquire)) return fn(c);
  return defaultPromptInputFor(c);
}

std::string defaultPromptInputFor(Control c) {
  // kernel/host_automation.cpp mapKeysToPad (the iter-450S keymap); where it maps two keys to
  // one control the prompt names the more natural one (Space for A, Escape for BACK).
  switch (c) {
    case kDpadUp: return "Up";
    case kDpadDown: return "Down";
    case kDpadLeft: return "Left";
    case kDpadRight: return "Right";
    case kStart: return "Return";
    case kBack: return "Escape";
    case kLB: return "Q";
    case kRB: return "E";
    case kA: return "Space";
    case kB: return "X";
    case kX: return "C";
    case kY: return "V";
    case kLStickUp: return "W";
    case kLStickDown: return "S";
    case kLStickLeft: return "A";
    case kLStickRight: return "D";
    default: return {};  // LT, RT, L3, R3, the right stick: not mapped by that keymap
  }
}

bool readGlyphPin(const std::string& text, GlyphSet* pin, bool* isAuto, std::string* errors) {
  std::istringstream in(text);
  std::string line;
  int lineNo = 0;
  bool found = false;
  while (std::getline(in, line)) {
    ++lineNo;
    if (lineNo == 1 && line.size() >= 3 && uint8_t(line[0]) == 0xEF && uint8_t(line[1]) == 0xBB &&
        uint8_t(line[2]) == 0xBF)
      line = line.substr(3);
    if (const size_t h = line.find('#'); h != std::string::npos) line.resize(h);
    line = trim(line);
    const size_t eq = line.find('=');
    if (line.empty() || line[0] == '[' || eq == std::string::npos) continue;
    if (!ieq(trim(line.substr(0, eq)), "glyphs")) continue;  // the controls lane's lines
    const std::string val = trim(line.substr(eq + 1));
    GlyphSet g{};
    bool a = false;
    if (parseGlyphSet(val.c_str(), &g, &a)) {
      *isAuto = a;
      if (!a) *pin = g;
      found = true;
    } else if (errors) {
      if (!errors->empty()) *errors += "; ";
      *errors += "line " + std::to_string(lineNo) + ": unknown glyphs '" + val + "'";
    }
  }
  return found;
}

// ── main thread ───────────────────────────────────────────────────────────────
void init() {
  State& s = S();
  std::lock_guard<std::mutex> lk(s.mu);
  if (s.inited) return;
  s.inited = true;
  readEnv(s);
  // The player's glyph pin (input.ini). Automated runs never read it: their content root is
  // a fresh scratch folder anyway (xam_content.hpp), and a roll must never depend on the
  // player's settings. SR_GLYPHS wins over the file.
  if (!automatedRun() && !std::getenv("SR_GLYPHS")) loadPlayerGlyphPin(s);
  if (!s.sdlPad) {
    SR_LOG_BOOT(sr::LogSeverity::Info,
                "[INPUT] SR_SDL_PAD=0: legacy XInputGetState(0) pad path, no hot-plug");
    return;
  }
  if (!s.realInput && !s.vpadOnly) {
    SR_LOG_BOOT(sr::LogSeverity::Info,
                "[INPUT] automated run: real controllers are not opened and real keyboard/mouse "
                "events do not pick player 1 (SR_HARNESS_REAL_INPUT=1 opts in); glyphs=%s",
                s.glyphs ? (s.glyphPinned ? glyphSetName(s.glyphPin) : "auto") : "off");
    return;
  }
  if (s.vpadOnly) {
    // RAWINPUT alone: no HIDAPI driver (it opens HID pads and writes LED / rumble reports to
    // them on discovery) and no XInput polling of the real pads. A ViGEm Xbox 360 pad still
    // enumerates with its HID device path, which the filter walks (probe in vpad_filter.hpp).
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI, "0");
    SDL_SetHint(SDL_HINT_XINPUT_ENABLED, "0");
    SDL_SetHint(SDL_HINT_JOYSTICK_RAWINPUT, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_RAWINPUT_CORRELATE_XINPUT, "0");
    // SDL3 Windows drivers SDL2 did not have (or had off) stay off: GameInput (default off outside
    // GDK) and WGI; the filter below only knows RAWINPUT's device paths.
    SDL_SetHint(SDL_HINT_JOYSTICK_GAMEINPUT, "0");
    SDL_SetHint(SDL_HINT_JOYSTICK_WGI, "0");
    SDL_SetHint(SDL_HINT_JOYSTICK_THREAD, "1");
    if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD)) {
      SR_LOG_BOOT(sr::LogSeverity::Warn, "[INPUT] SDL game controller init failed: %s", SDL_GetError());
      return;
    }
    SR_LOG_BOOT(sr::LogSeverity::Info,
                "[INPUT] automated run, SR_HARNESS_VPAD=1: only a ViGEm virtual pad is opened (its "
                "device tree reaches the ViGEmBus service); every other controller is ignored; "
                "real keyboard/mouse never pick player 1; glyphs=%s",
                s.glyphs ? (s.glyphPinned ? glyphSetName(s.glyphPin) : "auto") : "off");
    return;
  }
  // UnleashedRecomp hid::Init hints: HIDAPI drivers for PlayStation / Switch pads (rumble
  // and LEDs included), pads keep working while the window is in the background, and
  // POSITIONAL face buttons for Nintendo pads (the south button is the 360's A; its prompt
  // then shows the label printed on that button).
  // SDL3: positional face buttons are the only mapping (SDL_GAMEPAD_BUTTON_SOUTH/EAST/WEST/NORTH;
  // SDL2's GAMECONTROLLER_USE_BUTTON_LABELS=0 is gone), and SDL2's HIDAPI_PS4_RUMBLE /
  // HIDAPI_PS5_RUMBLE=1 are JOYSTICK_ENHANCED_REPORTS=1 (the enhanced report mode that carries
  // rumble on Bluetooth PlayStation pads).
  SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
  SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS4, "1");
  SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS5, "1");
  SDL_SetHint(SDL_HINT_JOYSTICK_ENHANCED_REPORTS, "1");
  SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS5_PLAYER_LED, "1");
  SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS3, "1");
  SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_SWITCH, "1");
  SDL_SetHint(SDL_HINT_XINPUT_ENABLED, "1");
  // Device arrival on its own thread: without it an XInput (XUSB) pad plugged in after start
  // was never enumerated in the bench (difftest --host-input-live + a ViGEm Xbox 360 pad: 0
  // connects; with the thread: connected in < 2 s, buttons and unplug seen). HID pads (DS4)
  // were found either way.
  SDL_SetHint(SDL_HINT_JOYSTICK_THREAD, "1");
  if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD)) {
    SR_LOG_BOOT(sr::LogSeverity::Warn, "[INPUT] SDL game controller init failed: %s",
                SDL_GetError());
    return;
  }
  // An optional community mapping database next to the exe (SDL's built-in one covers the
  // common pads; SDL_GAMECONTROLLERCONFIG is honoured by SDL itself).
  int added = 0;
  if (const char* base = SDL_GetBasePath()) {   // SDL3: SDL owns the string (SDL2: SDL_free)
    const std::string db = std::string(base) + "gamecontrollerdb.txt";
    std::error_code ec;
    if (std::filesystem::exists(db, ec)) added = SDL_AddGamepadMappingsFromFile(db.c_str());
  }
  SR_LOG_BOOT(sr::LogSeverity::Info,
              "[INPUT] SDL game controllers on (SDL %d.%d.%d; hot-plug; player 1 = last used "
              "device; extra mappings %d); glyphs=%s",
              SDL_VERSIONNUM_MAJOR(SDL_GetVersion()), SDL_VERSIONNUM_MINOR(SDL_GetVersion()),
              SDL_VERSIONNUM_MICRO(SDL_GetVersion()), added,
              s.glyphs ? (s.glyphPinned ? glyphSetName(s.glyphPin) : "auto") : "off");
}

void onEvent(const SDL_Event& e) {
  State& s = S();
  if (!s.inited || !s.sdlPad) return;
  switch (e.type) {
    case SDL_EVENT_GAMEPAD_ADDED: {
      if (!s.realInput && !s.vpadOnly) return;
      if (!s.realInput) {   // SR_HARNESS_VPAD: a ViGEm virtual pad or nothing
        // SDL3: the event carries the instance id (SDL2: a device index); the *ForID queries
        // read the same device before it is opened.
        const SDL_JoystickID jid = e.gdevice.which;
        const char* path = SDL_GetJoystickPathForID(jid);
        const uint16_t vid = SDL_GetJoystickVendorForID(jid), pid = SDL_GetJoystickProductForID(jid);
        const char* nm = SDL_GetJoystickNameForID(jid);
        std::string why;
        const bool ok = vpad::isVigemPad(path, vid, pid, &why);
        SR_LOG_BOOT(sr::LogSeverity::Info, "[INPUT] vpad filter: %s '%s' %04X:%04X (%s)",
                    ok ? "ACCEPTED" : "REJECTED (not opened)", nm ? nm : "?", vid, pid, why.c_str());
        if (!ok) return;
      }
      SDL_Gamepad* gc = SDL_OpenGamepad(e.gdevice.which);
      if (!gc) {
        SR_LOG_BOOT(sr::LogSeverity::Warn, "[INPUT] controller %u could not be opened: %s",
                    unsigned(e.gdevice.which), SDL_GetError());
        return;
      }
      Pad p;
      p.gc = gc;
      p.id = SDL_GetJoystickID(SDL_GetGamepadJoystick(gc));
      p.kind = kindFromSdl(gc);
      const char* nm = SDL_GetGamepadName(gc);
      p.name = nm ? nm : "unknown controller";
      std::lock_guard<std::mutex> lk(s.mu);
      if (findPad(s, p.id)) {  // SDL can repeat ADDED for an already-open device
        SDL_CloseGamepad(gc);
        return;
      }
      s.pads.push_back(p);
      SR_LOG_BOOT(sr::LogSeverity::Info,
                  "[INPUT] controller connected: '%s' type=%s id=%u (%zu connected)",
                  p.name.c_str(), deviceName(p.kind), unsigned(p.id), s.pads.size());
      // Nobody is player 1 yet, or player 1's pad was unplugged: this pad takes over.
      if (s.active == Device::None || s.p1Lost)
        setActive(s, p.kind, p.id, s.p1Lost ? "replaces the unplugged pad" : "first device");
      return;
    }
    case SDL_EVENT_GAMEPAD_REMOVED: {
      std::lock_guard<std::mutex> lk(s.mu);
      auto it = std::find_if(s.pads.begin(), s.pads.end(),
                             [&](const Pad& p) { return p.id == e.gdevice.which; });
      if (it == s.pads.end()) return;
      const std::string name = it->name;
      SDL_CloseGamepad(it->gc);
      s.pads.erase(it);
      const bool wasP1 = s.activeId == e.gdevice.which && s.active != Device::Keyboard;
      if (s.vibPad == e.gdevice.which) s.vibPad = kNoPad;
      if (wasP1) {
        s.activeId = kNoPad;
        s.p1Lost = true;  // the console's "controller removed" until something is used
        publish(s);
      }
      SR_LOG_BOOT(sr::LogSeverity::Info,
                  "[INPUT] controller disconnected: '%s' id=%u (%zu connected)%s", name.c_str(),
                  unsigned(e.gdevice.which), s.pads.size(),
                  wasP1 ? " - it was player 1: user 0 reports NOT CONNECTED until a device is "
                          "used or a pad is plugged in"
                        : "");
      return;
    }
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN: {
      std::lock_guard<std::mutex> lk(s.mu);
      if (const Pad* p = findPad(s, e.gbutton.which)) setActive(s, p->kind, p->id, "button");
      s.mouseAccum = 0.0f;
      return;
    }
    case SDL_EVENT_GAMEPAD_AXIS_MOTION: {
      const int v = e.gaxis.value;
      const bool trig = e.gaxis.axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER ||
                        e.gaxis.axis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER;
      if (trig ? v < kTriggerActivate : std::abs(v) < kStickActivate) return;
      std::lock_guard<std::mutex> lk(s.mu);
      if (const Pad* p = findPad(s, e.gaxis.which)) setActive(s, p->kind, p->id, "axis");
      s.mouseAccum = 0.0f;
      return;
    }
    // Keys: no event here. A key counts when it is MAPPED, i.e. when it changed the pad
    // report (stubs.cpp mergeKeyboardPad -> noteKeyboardUse), so an unbound key (a hotkey,
    // Alt+Tab, PrintScreen) never takes player 1 from a pad.
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_WHEEL: {
      if (!s.realInput || !hostWindowFocused()) return;
      std::lock_guard<std::mutex> lk(s.mu);
      setActive(s, Device::Keyboard, kNoPad, "mouse");
      return;
    }
    case SDL_EVENT_MOUSE_MOTION: {
      if (!s.realInput || !hostWindowFocused()) return;
      std::lock_guard<std::mutex> lk(s.mu);
      if (s.active == Device::Keyboard) return;
      s.mouseAccum += std::fabs(e.motion.xrel) + std::fabs(e.motion.yrel);
      if (s.mouseAccum > 48.0f) {
        s.mouseAccum = 0.0f;
        setActive(s, Device::Keyboard, kNoPad, "mouse");
      }
      return;
    }
    default:
      return;
  }
}

void tick() {
  State& s = S();
  if (!s.inited || !s.sdlPad || !(s.realInput || s.vpadOnly)) return;
  const uint32_t want = s.vib.load(std::memory_order_relaxed);
  std::lock_guard<std::mutex> lk(s.mu);
  const SDL_JoystickID target = s.p1Lost ? kNoPad : s.activeId;
  const uint32_t now = uint32_t(SDL_GetTicks());   // SDL3: Uint64 ms (the 1 s compare wraps as before)
  // Moved to another pad (or the keyboard): stop the old one first.
  if (s.vibPad != kNoPad && s.vibPad != target) {
    if (const Pad* p = findPad(s, s.vibPad)) SDL_RumbleGamepad(p->gc, 0, 0, 0);
    s.vibPad = kNoPad;
    s.vibApplied = 0;
  }
  if (target == kNoPad) return;
  const Pad* p = findPad(s, target);
  if (!p) return;
  // Re-send nonzero speeds every second with a 2 s duration: the console holds a speed until
  // the game changes it, SDL needs a duration (a lost update can never stick a motor on).
  const bool changed = want != s.vibApplied || s.vibPad != target;
  const bool refresh = want != 0 && now - s.vibLastMs >= 1000;
  if (!changed && !refresh) return;
  SDL_RumbleGamepad(p->gc, uint16_t(want >> 16), uint16_t(want & 0xFFFF),
                           want ? 2000 : 0);
  s.vibApplied = want;
  s.vibPad = target;
  s.vibLastMs = now;
}

void shutdown() {
  State& s = S();
  std::lock_guard<std::mutex> lk(s.mu);
  for (auto& p : s.pads) {
    SDL_RumbleGamepad(p.gc, 0, 0, 0);
    SDL_CloseGamepad(p.gc);
  }
  s.pads.clear();
  s.activeId = kNoPad;
}

// ── any thread ────────────────────────────────────────────────────────────────
bool sdlPadEnabled() {
  static const bool v = envOn("SR_SDL_PAD", true);
  return v;
}

bool realInputAllowed() {
  State& s = S();
  if (s.inited) return s.realInput;
  return envPolicy().realInput;
}

bool player1Connected() {
  return !S().p1LostAtomic.load(std::memory_order_relaxed);
}

bool readActivePad(PadState* out) {
  State& s = S();
  if (!s.inited || !s.sdlPad || !(s.realInput || s.vpadOnly)) return false;
  std::lock_guard<std::mutex> lk(s.mu);
  if (s.p1Lost || s.activeId == kNoPad || s.active == Device::Keyboard) return false;
  const Pad* p = findPad(s, s.activeId);
  if (!p) return false;
  SDL_Gamepad* gc = p->gc;
  auto btn = [&](SDL_GamepadButton b) {
    return SDL_GetGamepadButton(gc, b);
  };
  PadState st;
  if (btn(SDL_GAMEPAD_BUTTON_DPAD_UP)) st.buttons |= 0x0001;
  if (btn(SDL_GAMEPAD_BUTTON_DPAD_DOWN)) st.buttons |= 0x0002;
  if (btn(SDL_GAMEPAD_BUTTON_DPAD_LEFT)) st.buttons |= 0x0004;
  if (btn(SDL_GAMEPAD_BUTTON_DPAD_RIGHT)) st.buttons |= 0x0008;
  if (btn(SDL_GAMEPAD_BUTTON_START)) st.buttons |= 0x0010;
  // BACK: also the PlayStation touchpad click (UnleashedRecomp's mapping) - the Share /
  // Create button is awkward to reach and the touchpad is where a PS player expects "back".
  if (btn(SDL_GAMEPAD_BUTTON_BACK) || btn(SDL_GAMEPAD_BUTTON_TOUCHPAD)) st.buttons |= 0x0020;
  if (btn(SDL_GAMEPAD_BUTTON_LEFT_STICK)) st.buttons |= 0x0040;
  if (btn(SDL_GAMEPAD_BUTTON_RIGHT_STICK)) st.buttons |= 0x0080;
  if (btn(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER)) st.buttons |= 0x0100;
  if (btn(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER)) st.buttons |= 0x0200;
  if (btn(SDL_GAMEPAD_BUTTON_SOUTH)) st.buttons |= 0x1000;
  if (btn(SDL_GAMEPAD_BUTTON_EAST)) st.buttons |= 0x2000;
  if (btn(SDL_GAMEPAD_BUTTON_WEST)) st.buttons |= 0x4000;
  if (btn(SDL_GAMEPAD_BUTTON_NORTH)) st.buttons |= 0x8000;
  auto ax = [&](SDL_GamepadAxis a) { return int(SDL_GetGamepadAxis(gc, a)); };
  // Xenia sdl_input_driver.cc: Y axes inverted with ~v (maps -32768..32767 onto
  // 32767..-32768 exactly), triggers v >> 7 (0..32767 -> 0..255).
  st.lx = int16_t(ax(SDL_GAMEPAD_AXIS_LEFTX));
  st.ly = int16_t(~ax(SDL_GAMEPAD_AXIS_LEFTY));
  st.rx = int16_t(ax(SDL_GAMEPAD_AXIS_RIGHTX));
  st.ry = int16_t(~ax(SDL_GAMEPAD_AXIS_RIGHTY));
  st.lt = uint8_t(std::clamp(ax(SDL_GAMEPAD_AXIS_LEFT_TRIGGER), 0, 32767) >> 7);
  st.rt = uint8_t(std::clamp(ax(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER), 0, 32767) >> 7);
  *out = st;
  return true;
}

void setVibration(uint16_t left, uint16_t right) {
  S().vib.store((uint32_t(left) << 16) | right, std::memory_order_relaxed);
}

void noteKeyboardUse() {
  State& s = S();
  if (!s.inited) return;
  if (Device(s.activeAtomic.load(std::memory_order_relaxed)) == Device::Keyboard &&
      !s.p1LostAtomic.load(std::memory_order_relaxed))
    return;  // fast path: already the keyboard
  std::lock_guard<std::mutex> lk(s.mu);
  setActive(s, Device::Keyboard, kNoPad, "key/mouse (pad report)");
}

Device activeDevice() { return Device(S().activeAtomic.load(std::memory_order_relaxed)); }

GlyphSet activeGlyphSet() {
  State& s = S();
  if (s.glyphCycleMs)
    return GlyphSet(uint32_t((SDL_GetTicks() / s.glyphCycleMs) % uint32_t(GlyphSet::kCount)));
  if (s.glyphPinned) return s.glyphPin;
  return glyphSetForDevice(activeDevice());
}

bool glyphsEnabled() { return S().glyphs; }

// iter-466 gfx: the PC SETTINGS screen's BUTTON PROMPTS row changed input.ini - read the pin again
// (players only, as at init; SR_GLYPHS still wins). The glyph generation moves so the prompt
// textures follow at once.
void reloadGlyphPin() {
  if (automatedRun() || std::getenv("SR_GLYPHS")) return;
  State& s = S();
  std::lock_guard<std::mutex> lk(s.mu);
  s.glyphPinned = false;
  loadPlayerGlyphPin(s);
  publish(s);
}

uint32_t glyphGeneration() { return S().gen.load(std::memory_order_acquire); }

void resetForTest() {
  State& s = S();
  std::lock_guard<std::mutex> lk(s.mu);
  for (auto& p : s.pads) SDL_CloseGamepad(p.gc);
  s.pads.clear();
  s.activeId = kNoPad;
  s.active = Device::None;
  s.p1Lost = false;
  s.inited = false;
  s.mouseAccum = 0.0f;
  s.vib.store(0);
  s.vibApplied = 0;
  s.vibPad = kNoPad;
  s.changeLogs = 0;
  publish(s);
}

}  // namespace sr::kernel::hostinput
