// RELEASE_PLAN step 3 (the SDL3 platform port): what SDL2 gave the game that SDL3 changed, kept.
//
// Run: build/difftest.exe --sdl3-platform-test     (exit = failed checks)
//
//   K   key names: input.ini stores SDL key names. Every scancode SDL2 named below 258 has the same
//       name in SDL3, and SDL_GetScancodeFromName resolves it to the same code (SDL2's table,
//       2.32.10 src/events/SDL_keyboard.c, is embedded below). SDL2's media block (258+): the
//       renamed keys load as the same physical key through kernel/pc_controls_bindings.cpp
//       scancodeFromName, the kept names resolve as before, and exactly the keys SDL3 dropped
//       read as unknown. Every SDL3 key name round-trips through inputName / parseInputName.
//   F   the owner's input.ini (version 3, written by the SDL2 build on 2026-10-11), parsed over a
//       CLEARED base, loads with no problem into the default bindings; defaultIni() parses back to the
//       defaults; a player's own layout (keypad / GUI keys, SDL2 media names) loads as written.
//   D   display semantics (app/platform_sdl.hpp sdl3platform): the fallback integer refresh rate from
//       SDL3's float; SDL2's closest-mode rule (the lowest rate >= the request) where SDL3's own picks
//       the nearest; display_index -> list; SDL2's wheel ticks (a reversal drops the carry).
//   W   the window handle the D3D12 swap chain takes: a hidden SDL3 window's
//       SDL_PROP_WINDOW_WIN32_HWND_POINTER is a live HWND, and its size in window units equals its
//       size in pixels (Windows: SDL3 window coordinates are pixels, as SDL2's were with
//       per-monitor DPI awareness); W4 every real display mode's refresh is Windows' own integer
//       (the DEVMODE read, cross-checked with EnumDisplaySettingsW); W3 (with SR_INSTANCE_ID set at
//       process start) the production PlatformSDL::init window is shown without taking the foreground.
//       Skipped (not passed) where no video device / mode exists.
#include "app/platform_sdl.hpp"
#include "kernel/pc_controls.hpp"

#include <SDL3/SDL.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace {
int g_pass = 0, g_fail = 0;
void check(bool ok, const std::string& name) {
  if (ok) { g_pass++; std::printf("  ok  %s\n", name.c_str()); }
  else { g_fail++; std::printf("FAIL: %s\n", name.c_str()); }
}

struct Name {
  int sc;
  const char* name;
};
// SDL 2.32.10 SDL_scancode_names (every named entry; generated from the release tarball's
// src/events/SDL_keyboard.c by the lane's gen_sdl2_names.py).
const Name kSdl2Names[] = {
    {4, "A"}, {5, "B"}, {6, "C"}, {7, "D"}, {8, "E"}, {9, "F"}, {10, "G"}, {11, "H"}, {12, "I"}, {13, "J"},
    {14, "K"}, {15, "L"}, {16, "M"}, {17, "N"}, {18, "O"}, {19, "P"}, {20, "Q"}, {21, "R"}, {22, "S"},
    {23, "T"}, {24, "U"}, {25, "V"}, {26, "W"}, {27, "X"}, {28, "Y"}, {29, "Z"}, {30, "1"}, {31, "2"},
    {32, "3"}, {33, "4"}, {34, "5"}, {35, "6"}, {36, "7"}, {37, "8"}, {38, "9"}, {39, "0"}, {40, "Return"},
    {41, "Escape"}, {42, "Backspace"}, {43, "Tab"}, {44, "Space"}, {45, "-"}, {46, "="}, {47, "["}, {48, "]"},
    {49, "\\"}, {50, "#"}, {51, ";"}, {52, "'"}, {53, "`"}, {54, ","}, {55, "."}, {56, "/"}, {57, "CapsLock"},
    {58, "F1"}, {59, "F2"}, {60, "F3"}, {61, "F4"}, {62, "F5"}, {63, "F6"}, {64, "F7"}, {65, "F8"},
    {66, "F9"}, {67, "F10"}, {68, "F11"}, {69, "F12"}, {70, "PrintScreen"}, {71, "ScrollLock"}, {72, "Pause"},
    {73, "Insert"}, {74, "Home"}, {75, "PageUp"}, {76, "Delete"}, {77, "End"}, {78, "PageDown"},
    {79, "Right"}, {80, "Left"}, {81, "Down"}, {82, "Up"}, {83, "Numlock"}, {84, "Keypad /"},
    {85, "Keypad *"}, {86, "Keypad -"}, {87, "Keypad +"}, {88, "Keypad Enter"}, {89, "Keypad 1"},
    {90, "Keypad 2"}, {91, "Keypad 3"}, {92, "Keypad 4"}, {93, "Keypad 5"}, {94, "Keypad 6"},
    {95, "Keypad 7"}, {96, "Keypad 8"}, {97, "Keypad 9"}, {98, "Keypad 0"}, {99, "Keypad ."},
    {101, "Application"}, {102, "Power"}, {103, "Keypad ="}, {104, "F13"}, {105, "F14"}, {106, "F15"},
    {107, "F16"}, {108, "F17"}, {109, "F18"}, {110, "F19"}, {111, "F20"}, {112, "F21"}, {113, "F22"},
    {114, "F23"}, {115, "F24"}, {116, "Execute"}, {117, "Help"}, {118, "Menu"}, {119, "Select"},
    {120, "Stop"}, {121, "Again"}, {122, "Undo"}, {123, "Cut"}, {124, "Copy"}, {125, "Paste"}, {126, "Find"},
    {127, "Mute"}, {128, "VolumeUp"}, {129, "VolumeDown"}, {133, "Keypad ,"}, {134, "Keypad = (AS400)"},
    {153, "AltErase"}, {154, "SysReq"}, {155, "Cancel"}, {156, "Clear"}, {157, "Prior"}, {158, "Return"},
    {159, "Separator"}, {160, "Out"}, {161, "Oper"}, {162, "Clear / Again"}, {163, "CrSel"}, {164, "ExSel"},
    {176, "Keypad 00"}, {177, "Keypad 000"}, {178, "ThousandsSeparator"}, {179, "DecimalSeparator"},
    {180, "CurrencyUnit"}, {181, "CurrencySubUnit"}, {182, "Keypad ("}, {183, "Keypad )"}, {184, "Keypad {"},
    {185, "Keypad }"}, {186, "Keypad Tab"}, {187, "Keypad Backspace"}, {188, "Keypad A"}, {189, "Keypad B"},
    {190, "Keypad C"}, {191, "Keypad D"}, {192, "Keypad E"}, {193, "Keypad F"}, {194, "Keypad XOR"},
    {195, "Keypad ^"}, {196, "Keypad %"}, {197, "Keypad <"}, {198, "Keypad >"}, {199, "Keypad &"},
    {200, "Keypad &&"}, {201, "Keypad |"}, {202, "Keypad ||"}, {203, "Keypad :"}, {204, "Keypad #"},
    {205, "Keypad Space"}, {206, "Keypad @"}, {207, "Keypad !"}, {208, "Keypad MemStore"},
    {209, "Keypad MemRecall"}, {210, "Keypad MemClear"}, {211, "Keypad MemAdd"}, {212, "Keypad MemSubtract"},
    {213, "Keypad MemMultiply"}, {214, "Keypad MemDivide"}, {215, "Keypad +/-"}, {216, "Keypad Clear"},
    {217, "Keypad ClearEntry"}, {218, "Keypad Binary"}, {219, "Keypad Octal"}, {220, "Keypad Decimal"},
    {221, "Keypad Hexadecimal"}, {224, "Left Ctrl"}, {225, "Left Shift"}, {226, "Left Alt"},
    {227, "Left GUI"}, {228, "Right Ctrl"}, {229, "Right Shift"}, {230, "Right Alt"}, {231, "Right GUI"},
    {257, "ModeSwitch"}, {258, "AudioNext"}, {259, "AudioPrev"}, {260, "AudioStop"}, {261, "AudioPlay"},
    {262, "AudioMute"}, {263, "MediaSelect"}, {264, "WWW"}, {265, "Mail"}, {266, "Calculator"},
    {267, "Computer"}, {268, "AC Search"}, {269, "AC Home"}, {270, "AC Back"}, {271, "AC Forward"},
    {272, "AC Stop"}, {273, "AC Refresh"}, {274, "AC Bookmarks"}, {275, "BrightnessDown"},
    {276, "BrightnessUp"}, {277, "DisplaySwitch"}, {278, "KBDIllumToggle"}, {279, "KBDIllumDown"},
    {280, "KBDIllumUp"}, {281, "Eject"}, {282, "Sleep"}, {283, "App1"}, {284, "App2"}, {285, "AudioRewind"},
    {286, "AudioFastForward"}, {287, "SoftLeft"}, {288, "SoftRight"}, {289, "Call"}, {290, "EndCall"},
};

// The owner's settings/input.ini as the SDL2 build wrote it (2026-10-11 18:50, version 3).
const char kOwnerInputIni[] =
    "# Superman Returns (recomp) - PC controls. docs/PC_CONTROLS.md explains every line.\n"
    "# action = input[, input ...]   (up to four; 'none' unbinds). PC SETTINGS > CONTROLS edits it.\n"
    "# A key IS the game's action: it drives the game's own input commands (docs/PC_CONTROLS.md 2).\n"
    "# inputs: SDL key names (W, Space, Left Shift, Left Ctrl, Left Alt, Tab, Return, 1 ...),\n"
    "#         Mouse1 (left) Mouse2 (right) Mouse3 (middle) Mouse4 Mouse5 WheelUp WheelDown\n"
    "# A controller always works too; the game's own CONTROLLER OPTIONS (configuration,\n"
    "# X/Y inversion, smart camera) apply to the mouse and keyboard as well.\n"
    "version = 3\n"
    "\n"
    "[controls]\n"
    "MoveForward = W\nMoveBack = S\nMoveLeft = A\nMoveRight = D\nAttack = Mouse1\nBlock = Mouse2\n"
    "Pickup = E\nTakeoff = Space\nSuperspeed = Left Shift\nTargetLock = Mouse3, Tab\nSuperpower = F\n"
    "CameraHover = Left Ctrl\nMap = M\nPause = Escape\nPowerUp = 1\nPowerRight = 2\nPowerLeft = 3\n"
    "PowerDown = none\nPowerNext = WheelDown\nPowerPrev = WheelUp\nDodge = Left Alt\nLookUp = none\n"
    "LookDown = none\nLookLeft = none\nLookRight = none\nMenuUp = Up\nMenuDown = Down\nMenuLeft = Left\n"
    "MenuRight = Right\nMenuConfirm = Return, Keypad Enter\nMenuBack = Escape, Backspace\nMenuX = E\n"
    "MenuY = F\nCycleTarget = Q\n"
    "\n"
    "[mouse]\n"
    "# mouse-look in gameplay (the menus get the mouse cursor)\n"
    "look = on\n"
    "# 1.0 = the default speed; 2.0 turns twice as far per mouse movement\n"
    "sensitivity = 1.0\n"
    "# extra inversion ON TOP of the game's own X / Y INVERSION options (normally off)\n"
    "invert_x = off\n"
    "invert_y = off\n"
    "\n"
    "[glyphs]\n"
    "# button prompts: auto (follow the device you last used) | xbox | ps4 | ps5 | switch | keyboard\n"
    "glyphs = auto\n";

bool sameBindings(const sr::kernel::pccontrols::Bindings& a, const sr::kernel::pccontrols::Bindings& b) {
  return std::memcmp(&a.in, &b.in, sizeof(a.in)) == 0;
}

void keyNames() {
  namespace PC = sr::kernel::pccontrols;
  // SDL2's own FromName: the first code with that name (both tables list "Return" at 40 and 158).
  std::map<std::string, int> sdl2From;
  for (const Name& n : kSdl2Names) sdl2From.emplace(n.name, n.sc);
  int same = 0, kept = 0;
  std::string bad;
  for (const Name& n : kSdl2Names) {
    if (n.sc >= 258) continue;
    const char* now = SDL_GetScancodeName(SDL_Scancode(n.sc));
    const bool nameOk = now && std::strcmp(now, n.name) == 0;
    const bool fromOk = int(SDL_GetScancodeFromName(n.name)) == sdl2From[n.name];
    uint16_t parsed = 0;
    const bool iniOk = PC::parseInputName(n.name, &parsed) && parsed == sdl2From[n.name];
    if (nameOk && fromOk && iniOk) ++same;
    else bad += std::string(" ") + std::to_string(n.sc) + "'" + n.name + "'";
    ++kept;
  }
  check(same == kept && kept == 194,
        "K1 every SDL2 key name below 258 (" + std::to_string(kept) + ") is SDL3's name for the same code, "
        "and SDL_GetScancodeFromName / input.ini resolve it as SDL2 did" + (bad.empty() ? "" : ":" + bad));

  // The media block: renamed -> the same key; kept names -> found; dropped keys -> unknown.
  const std::map<std::string, SDL_Scancode> renamed = {
      {"AudioNext", SDL_SCANCODE_MEDIA_NEXT_TRACK}, {"AudioPrev", SDL_SCANCODE_MEDIA_PREVIOUS_TRACK},
      {"AudioStop", SDL_SCANCODE_MEDIA_STOP},       {"AudioPlay", SDL_SCANCODE_MEDIA_PLAY_PAUSE},
      {"AudioMute", SDL_SCANCODE_MUTE},             {"AudioRewind", SDL_SCANCODE_MEDIA_REWIND},
      {"AudioFastForward", SDL_SCANCODE_MEDIA_FAST_FORWARD},
  };
  const std::set<std::string> dropped = {"WWW", "Mail", "Calculator", "Computer", "BrightnessDown",
                                         "BrightnessUp", "DisplaySwitch", "KBDIllumToggle", "KBDIllumDown",
                                         "KBDIllumUp", "App1", "App2"};
  int renamedOk = 0, keptOk = 0, droppedOk = 0, media = 0;
  std::string mbad;
  for (const Name& n : kSdl2Names) {
    if (n.sc < 258) continue;
    ++media;
    uint16_t v = 0;
    const bool ok = PC::parseInputName(n.name, &v);
    if (auto it = renamed.find(n.name); it != renamed.end()) {
      if (ok && v == uint16_t(it->second)) ++renamedOk; else mbad += std::string(" ") + n.name;
    } else if (dropped.count(n.name)) {
      if (!ok) ++droppedOk; else mbad += std::string(" ") + n.name;
    } else {
      const char* back = ok ? SDL_GetScancodeName(SDL_Scancode(v)) : nullptr;
      if (ok && back && std::strcmp(back, n.name) == 0) ++keptOk; else mbad += std::string(" ") + n.name;
    }
  }
  check(renamedOk == 7 && droppedOk == 12 && keptOk == 14 && media == 33,
        "K2 SDL2's media keys: " + std::to_string(renamedOk) + " renamed load as the same key (AudioNext -> "
        "MediaTrackNext ...), " + std::to_string(keptOk) + " kept names resolve, " + std::to_string(droppedOk) +
        " keys SDL3 dropped read as unknown" + (mbad.empty() ? "" : "; WRONG:" + mbad));

  // Every SDL3 name: inputName -> parseInputName -> a code with that name.
  int named = 0, round = 0;
  for (int sc = 1; sc < SDL_SCANCODE_COUNT; ++sc) {
    const char* nm = SDL_GetScancodeName(SDL_Scancode(sc));
    if (!nm || !*nm) continue;
    ++named;
    const std::string s = PC::inputName(uint16_t(sc));
    uint16_t v = 0;
    if (s == nm && PC::parseInputName(s.c_str(), &v) && std::strcmp(SDL_GetScancodeName(SDL_Scancode(v)), nm) == 0)
      ++round;
  }
  check(named == round && named >= 247,
        "K3 all " + std::to_string(named) + " SDL3 key names round-trip through input.ini (inputName -> parseInputName)");
}

void ownerIni() {
  namespace PC = sr::kernel::pccontrols;
  // The base the file is parsed over is NOT the defaults (every binding cleared, the mouse options flipped),
  // so only lines the parser really read can produce the defaults.
  PC::Config base;
  base.bindings = PC::Bindings{};
  base.mouse.look = !base.mouse.look;
  base.mouse.sensitivity = 3.5f;
  base.mouse.invertX = !base.mouse.invertX;
  base.mouse.invertY = !base.mouse.invertY;
  std::string err;
  const PC::Config c = PC::parseIni(kOwnerInputIni, base, &err);
  const PC::Config d{};
  check(err.empty() && sameBindings(c.bindings, PC::defaultBindings()) && c.mouse.look == d.mouse.look &&
            c.mouse.sensitivity == d.mouse.sensitivity && c.mouse.invertX == d.mouse.invertX &&
            c.mouse.invertY == d.mouse.invertY,
        "F1 the owner's input.ini (version 3, written by the SDL2 build), parsed over a cleared base, loads with no "
        "problem into the default bindings and mouse options" + (err.empty() ? std::string() : "; problems: " + err));
  err.clear();
  const PC::Config r = PC::parseIni(PC::defaultIni(), base, &err);
  check(err.empty() && sameBindings(r.bindings, PC::defaultBindings()),
        "F2 the file the SDL3 build writes for a new player (defaultIni) parses back to the defaults");
  // A player's own layout: SDL key names incl. SDL2's media names and a keypad key.
  std::string mine = kOwnerInputIni;
  auto swap = [&](const std::string& a, const std::string& b) {
    const size_t at = mine.find(a);
    if (at != std::string::npos) mine.replace(at, a.size(), b);
  };
  swap("Attack = Mouse1\n", "Attack = Keypad 0, AudioPlay\n");
  swap("Map = M\n", "Map = AudioNext, Right GUI\n");
  err.clear();
  const PC::Config m = PC::parseIni(mine, base, &err);
  check(err.empty() && m.bindings.in[PC::kAttack][0] == SDL_SCANCODE_KP_0 &&
            m.bindings.in[PC::kAttack][1] == SDL_SCANCODE_MEDIA_PLAY_PAUSE &&
            m.bindings.in[PC::kMap][0] == SDL_SCANCODE_MEDIA_NEXT_TRACK && m.bindings.in[PC::kMap][1] == SDL_SCANCODE_RGUI,
        "F3 a player's own bindings load as written (Keypad 0, Right GUI; SDL2's AudioPlay / AudioNext as the same keys)");
}

void display() {
  namespace S3 = sr::sdl3platform;
  check(S3::sdl2RefreshHz(60.0f) == 60 && S3::sdl2RefreshHz(144.0f) == 144 && S3::sdl2RefreshHz(240.0f) == 240,
        "D1 integral refresh rates are unchanged (60, 144, 240)");
  check(S3::sdl2RefreshHz(59.94f) == 59 && S3::sdl2RefreshHz(119.88f) == 119 && S3::sdl2RefreshHz(29.97f) == 29,
        "D2 SDL3's NTSC rates read as Windows' / SDL2's integer (59.94 -> 59, 119.88 -> 119, 29.97 -> 29)");
  check(S3::sdl2RefreshHz(59.95f) == 60 && S3::sdl2RefreshHz(143.98f) == 144 && S3::sdl2RefreshHz(165.01f) == 165,
        "D3 a DXGI-refined rate near an integer rounds (59.95 -> 60, 143.98 -> 144, 165.01 -> 165)");
  check(S3::sdl2RefreshHz(0.0f) == 0, "D4 0 = unknown stays 0");

  const uint32_t x8 = SDL_PIXELFORMAT_XRGB8888, r565 = SDL_PIXELFORMAT_RGB565;
  // SDL3's order (SDL_GetFullscreenDisplayModes): largest first, then depth, then refresh.
  const std::vector<S3::ModeCandidate> modes = {
      {3840, 2160, 60, x8},  {2560, 1440, 144, x8}, {2560, 1440, 120, x8}, {2560, 1440, 60, x8},
      {2560, 1080, 60, x8},  {1920, 1200, 60, x8},  {1920, 1080, 240, x8}, {1920, 1080, 144, x8},
      {1920, 1080, 60, x8},  {1920, 1080, 59, x8},  {1920, 1080, 60, r565}, {1280, 720, 60, x8},
  };
  auto pick = [&](int w, int h, int hz, int desk) {
    const int k = S3::closestModeSdl2(modes.data(), int(modes.size()), w, h, hz, desk, x8);
    return k < 0 ? std::string("none") : std::to_string(modes[size_t(k)].w) + "x" + std::to_string(modes[size_t(k)].h) +
                                             "@" + std::to_string(modes[size_t(k)].hz);
  };
  check(pick(1920, 1080, 0, 144) == "1920x1080@144", "D5 0 Hz asks for the desktop's rate (144)");
  check(pick(1920, 1080, 100, 144) == "1920x1080@144",
        "D6 100 Hz: the LOWEST rate >= 100 (144), SDL2's rule - SDL3's own call would pick 60 (nearer)");
  check(pick(1920, 1080, 300, 60) == "1920x1080@240", "D7 above every rate: the highest (240)");
  check(pick(1920, 1080, 60, 144) == "1920x1080@60" && pick(1920, 1080, 59, 144) == "1920x1080@59",
        "D8 an exact rate is kept (60; the NTSC 59 entry too)");
  check(pick(2000, 1100, 0, 144) == "2560x1440@144", "D9 no exact size: the smallest at least that large");
  check(pick(1920, 1200, 0, 60) == "1920x1200@60",
        "D10 another aspect ratio that is wider but too short (2560x1080) is skipped, not taken");
  check(pick(4000, 3000, 0, 60) == "none", "D11 larger than every mode: none (the caller falls back to the desktop)");
  check(modes[size_t(S3::closestModeSdl2(modes.data(), int(modes.size()), 1920, 1080, 60, 60, x8))].format == x8,
        "D12 at equal size and rate the desktop's pixel format wins over RGB565");
  check(S3::displayIndexOr0(1, 2) == 1 && S3::displayIndexOr0(2, 2) == 0 && S3::displayIndexOr0(-1, 3) == 0,
        "D13 display_index: in range = that display, else the primary (index 0)");
  {
    float acc = 0.0f;
    const int a = S3::sdl2WheelStep(0.75f, &acc), b = S3::sdl2WheelStep(-0.5f, &acc), c = S3::sdl2WheelStep(-0.5f, &acc);
    float accUp = 0.0f;   // the other direction: a reversal upward drops a downward carry too
    const int ua = S3::sdl2WheelStep(-0.75f, &accUp), ub = S3::sdl2WheelStep(0.5f, &accUp),
              uc = S3::sdl2WheelStep(0.5f, &accUp);
    float acc2 = 0.0f;
    int notched = 0;
    for (float v : {1.0f, 1.0f, -1.0f, 2.0f}) notched += S3::sdl2WheelStep(v, &acc2) == int(v) ? 1 : 0;
    float acc3 = 0.0f;
    int fine = 0;
    for (int i = 0; i < 8; ++i) fine += S3::sdl2WheelStep(0.25f, &acc3);
    check(a == 0 && b == 0 && c == -1 && ua == 0 && ub == 0 && uc == 1 && notched == 4 && fine == 2,
          "D14 wheel ticks as SDL2 counted them: +0.75, -0.5, -0.5 -> 0, 0, -1 and -0.75, +0.5, +0.5 -> 0, 0, +1 (a "
          "reversal drops the carry; SDL3's integer_y gives 0, 0, 0); whole notches pass through; eight quarter "
          "notches = 2 ticks");
  }
}

void window() {
  if (!SDL_InitSubSystem(SDL_INIT_VIDEO)) {
    std::printf("  SKIP W: no SDL video device here (%s) - not counted\n", SDL_GetError());
    return;
  }
  SDL_Window* w = SDL_CreateWindow("sdl3-platform-test", 640, 360, SDL_WINDOW_HIDDEN);
  if (!w) {
    std::printf("  SKIP W: SDL_CreateWindow failed (%s) - not counted\n", SDL_GetError());
    SDL_QuitSubSystem(SDL_INIT_VIDEO);
    return;
  }
  void* hwnd = SDL_GetPointerProperty(SDL_GetWindowProperties(w), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
#ifdef _WIN32
  check(hwnd && IsWindow(static_cast<HWND>(hwnd)) && !IsWindowVisible(static_cast<HWND>(hwnd)),
        "W1 SDL_PROP_WINDOW_WIN32_HWND_POINTER is the window's live HWND (hidden: nothing shown)");
#else
  (void)hwnd;
#endif
  int ww = 0, wh = 0, pw = 0, ph = 0;
  SDL_GetWindowSize(w, &ww, &wh);
  SDL_GetWindowSizeInPixels(w, &pw, &ph);
  check(ww == 640 && wh == 360 && pw == ww && ph == wh,
        "W2 window size = pixel size (" + std::to_string(pw) + "x" + std::to_string(ph) + "): SDL3 window units are pixels on Windows");
  SDL_DestroyWindow(w);

  // W4: windowsRefreshHz reads Windows' own integer for every mode of every display: the DEVMODE SDL3 kept
  // is recognised (never the fallback), and its rate is one EnumDisplaySettingsW lists for that size.
#ifdef _WIN32
  int modes = 0, exact = 0, listed = 0;
  int nd = 0;
  if (SDL_DisplayID* ids = SDL_GetDisplays(&nd)) {
    for (int i = 0; i < nd; ++i) {
      HMONITOR mon = static_cast<HMONITOR>(
          SDL_GetPointerProperty(SDL_GetDisplayProperties(ids[i]), SDL_PROP_DISPLAY_WINDOWS_HMONITOR_POINTER, nullptr));
      MONITORINFOEXW mi{};
      mi.cbSize = sizeof(mi);
      const bool haveName = mon && GetMonitorInfoW(mon, &mi);
      int nm = 0;
      if (SDL_DisplayMode** ms = SDL_GetFullscreenDisplayModes(ids[i], &nm)) {
        for (int k = 0; k < nm; ++k) {
          bool ex = false;
          const int hz = sr::sdl3platform::windowsRefreshHz(*ms[k], &ex);
          ++modes;
          exact += ex ? 1 : 0;
          if (!ex && modes - exact == 1) {   // the first mode the DEVMODE read refused: say why
            const DEVMODEW* raw = reinterpret_cast<const DEVMODEW*>(ms[k]->internal);
            std::printf("    W4 first fallback: mode %dx%d @ %.2f, internal %s, dmSize %u (want %u), dmPels %lux%lu, freq %lu\n",
                        ms[k]->w, ms[k]->h, double(ms[k]->refresh_rate), raw ? "set" : "NULL", raw ? unsigned(raw->dmSize) : 0u,
                        unsigned(sizeof(DEVMODEW)), raw ? raw->dmPelsWidth : 0ul, raw ? raw->dmPelsHeight : 0ul,
                        raw ? raw->dmDisplayFrequency : 0ul);
          }
          bool found = false;
          DEVMODEW dm{};
          dm.dmSize = sizeof(dm);
          for (DWORD j = 0; haveName && EnumDisplaySettingsW(mi.szDevice, j, &dm); ++j)
            if (int(dm.dmPelsWidth) == ms[k]->w && int(dm.dmPelsHeight) == ms[k]->h && int(dm.dmDisplayFrequency) == hz)
              found = true;
          listed += found ? 1 : 0;
        }
        SDL_free(ms);
      }
    }
    SDL_free(ids);
  }
  if (modes == 0) {
    std::printf("  SKIP W4: no display modes here - not counted\n");
  } else {
    check(exact == modes && listed == modes,
          "W4 every one of " + std::to_string(modes) + " display modes: the refresh is Windows' own dmDisplayFrequency (" +
              std::to_string(exact) + " read exactly, " + std::to_string(listed) + " listed by EnumDisplaySettingsW)");
  }
#endif
  SDL_QuitSubSystem(SDL_INIT_VIDEO);

  // W3: the production window path (PlatformSDL::init, the replay modes' fixed window) on an automated run:
  // shown WITHOUT activation, its HWND the D3D12 swap chain's, client = 1280x720 pixels.
#ifdef _WIN32
  {
    const char* had = std::getenv("SR_INSTANCE_ID");
    if (!had || !*had) {
      std::printf("  SKIP W3: needs SR_INSTANCE_ID set before the process starts (the automation policy is read once) "
                  "- run as: set SR_INSTANCE_ID=sdl3test && difftest --sdl3-platform-test - not counted\n");
    } else {
      HWND before = GetForegroundWindow();
      sr::PlatformSDL p;
      if (!p.init(1280, 720, /*skipRenderer=*/true)) {
        check(false, std::string("W3 PlatformSDL::init failed: ") + SDL_GetError());
      } else {
        HWND hwnd = static_cast<HWND>(p.nativeWindowHandle());
        SDL_PumpEvents();
        RECT rc{};
        // The window's OWN WS_VISIBLE style, not IsWindowVisible: that also needs every ancestor visible, and in a
        // non-interactive session (the worker over ssh) the desktop window is not, so it reads 0 for any window.
        // There the foreground is NULL before and after too; the no-activation half bites on a real desktop.
        const bool shown = hwnd && (GetWindowLongW(hwnd, GWL_STYLE) & WS_VISIBLE) != 0;
        const bool live = hwnd && IsWindow(hwnd) && shown && GetClientRect(hwnd, &rc);
        const HWND after = GetForegroundWindow();
        const bool notForeground = after != hwnd && (before == nullptr || after == before);
        const bool ok = live && rc.right - rc.left == 1280 && rc.bottom - rc.top == 720 && notForeground &&
                        p.getWindowWidth() == 1280 && p.getWindowHeight() == 720;
        if (!ok)
          std::printf("    W3 detail: hwnd %p IsWindow %d visible %d client %ldx%ld, platform %dx%d, foreground before %p "
                      "after %p, style %08lx exstyle %08lx\n",
                      static_cast<void*>(hwnd), hwnd ? int(IsWindow(hwnd)) : 0, hwnd ? int(IsWindowVisible(hwnd)) : 0,
                      long(rc.right - rc.left), long(rc.bottom - rc.top), p.getWindowWidth(), p.getWindowHeight(),
                      static_cast<void*>(before), static_cast<void*>(after),
                      hwnd ? static_cast<unsigned long>(GetWindowLongW(hwnd, GWL_STYLE)) : 0ul,
                      hwnd ? static_cast<unsigned long>(GetWindowLongW(hwnd, GWL_EXSTYLE)) : 0ul);
        check(ok,
              "W3 PlatformSDL on an automated run: the window is shown, its HWND (SDL_PROP_WINDOW_WIN32_HWND_POINTER) "
              "is live with a 1280x720 pixel client area, and the foreground window is unchanged (no activation)");
        p.shutdown();
      }
    }
  }
#endif
}
}  // namespace

int runSdl3PlatformTest() {
  std::printf("[SDL3_PLATFORM_TEST] SDL %d.%d.%d: key names, input.ini, display semantics, window handle\n",
              SDL_VERSIONNUM_MAJOR(SDL_GetVersion()), SDL_VERSIONNUM_MINOR(SDL_GetVersion()),
              SDL_VERSIONNUM_MICRO(SDL_GetVersion()));
  keyNames();
  ownerIni();
  display();
  window();
  std::printf("[SDL3_PLATFORM_TEST] %d passed, %d failed\n", g_pass, g_fail);
  return g_fail;
}
