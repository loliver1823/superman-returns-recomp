// iter-464 bgruns: automated runs never take the foreground (kernel/host_automation.{hpp,cpp}).
//
// Run: build/difftest.exe --host-automation-test     (exit = failed checks)
//
// What it proves:
//   P   the policy table (computePolicy): a player (no SR_INSTANCE_ID) keeps the foreground
//       window and real input and has no harness channel; an automated run (SR_INSTANCE_ID)
//       is background + harness keyboard + real input ignored; each SR_* override flips only
//       its own field, '0' = off, empty = unset;
//   N   the channel name: Local\SR_HARNESS_INPUT_<id>, id sanitised, "default" when empty;
//   M   mapKeysToPad is the iter-450S table, key by key (12 buttons, 4 stick directions,
//       the two two-key buttons), and nothing else maps;
//   C   the REAL channel, opened BY NAME the way the harness opens it (OpenFileMapping):
//       magic/version/pid stamped, keys zeroed even when a stale mapping pre-existed with a
//       held key, a harness byte is visible through harnessKeys(), harnessNoteMerge publishes
//       the poll count and mask.
#include "kernel/host_automation.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <SDL3/SDL.h>
#ifdef _WIN32
#include <windows.h>
#endif

namespace A = sr::kernel::automation;

namespace {
int g_pass = 0, g_fail = 0;
void check(bool ok, const char* name) {
  if (ok) { g_pass++; std::printf("  ok  %s\n", name); }
  else { g_fail++; std::printf("FAIL: %s\n", name); }
}
bool eq(const A::Policy& p, bool automated, bool bg, bool harness, bool real) {
  return p.automated == automated && p.windowBackground == bg && p.harnessInput == harness &&
         p.realInput == real;
}
}  // namespace

int runHostAutomationTest() {
  std::printf("--host-automation-test (kernel/host_automation.cpp)\n");

  // ---- P: the policy table ----
  check(eq(A::computePolicy(nullptr, false, nullptr, nullptr, nullptr), false, false, false, true),
        "P1 player (nothing set): foreground window, no harness channel, real input");
  check(eq(A::computePolicy("", false, nullptr, nullptr, nullptr), false, false, false, true),
        "P2 empty SR_INSTANCE_ID is unset (a player)");
  check(eq(A::computePolicy("run0", false, nullptr, nullptr, nullptr), true, true, true, false),
        "P3 automated (SR_INSTANCE_ID=run0): background + harness keyboard + real input ignored");
  check(eq(A::computePolicy("run0", false, "0", nullptr, nullptr), true, false, true, false),
        "P4 SR_WINDOW_BACKGROUND=0 restores the foreground window only");
  check(eq(A::computePolicy(nullptr, false, "1", nullptr, nullptr), false, true, false, true),
        "P5 SR_WINDOW_BACKGROUND=1 backgrounds a player window only");
  check(eq(A::computePolicy("run0", false, nullptr, "0", nullptr), true, true, false, false),
        "P6 SR_HARNESS_INPUT=0 closes the channel only");
  check(eq(A::computePolicy("run0", false, nullptr, nullptr, "1"), true, true, true, true),
        "P7 SR_HARNESS_REAL_INPUT=1 lets real input back into an automated run");
  check(eq(A::computePolicy(nullptr, false, nullptr, nullptr, "0"), false, false, false, false),
        "P8 SR_HARNESS_REAL_INPUT=0 isolates a player too (explicit)");
  check(eq(A::computePolicy("run0", false, "", "", ""), true, true, true, false),
        "P9 empty override values are unset");
  check(eq(A::computePolicy("run0", false, "0", "1", "1"), true, false, true, true),
        "P10 sr_drive -ForegroundInput shape (bg=0, real=1): foreground + both keyboards");
  check(eq(A::computePolicy(nullptr, true, nullptr, nullptr, nullptr), false, true, false, true),
        "P11 a trace replay (no instance) is background, nothing else changes");
  check(eq(A::computePolicy(nullptr, true, "0", nullptr, nullptr), false, false, false, true),
        "P12 SR_WINDOW_BACKGROUND=0 shows a replay in front (to watch it)");

  // ---- N: channel name ----
  // iter-465 player: THE PORT-FEATURE SWITCH (portFeaturesFor): players on, rolls off,
  // SR_PORT_FEATURES overrides both ways.
  check(A::portFeaturesFor(nullptr, nullptr), "F1 a player (no SR_INSTANCE_ID) gets the port features");
  check(A::portFeaturesFor("", nullptr), "F2 an empty SR_INSTANCE_ID is a player");
  check(!A::portFeaturesFor("run0", nullptr), "F3 an automated roll is natural (port features off)");
  check(A::portFeaturesFor("run0", "1"), "F4 SR_PORT_FEATURES=1 makes a roll a PORT FEATURE roll");
  check(!A::portFeaturesFor(nullptr, "0"), "F5 SR_PORT_FEATURES=0 turns them off for a player (A/B)");
  check(!A::portFeaturesFor("run0", "0"), "F6 SR_PORT_FEATURES=0 in a roll stays off");
  check(!A::portFeaturesFor("run0", ""), "F7 an empty SR_PORT_FEATURES is unset");

  check(A::harnessChannelName("run0") == "Local\\SR_HARNESS_INPUT_run0", "N1 run0");
  check(A::harnessChannelName(nullptr) == "Local\\SR_HARNESS_INPUT_default", "N2 unset -> default");
  check(A::harnessChannelName("") == "Local\\SR_HARNESS_INPUT_default", "N3 empty -> default");
  check(A::harnessChannelName("a b\\c/d:e.f-g_h") == "Local\\SR_HARNESS_INPUT_a_b_c_d_e.f-g_h",
        "N4 sanitised to [A-Za-z0-9_.-]");

  // ---- M: the mapping table ----
  struct Row { int sc; uint16_t mask; const char* name; };
  const Row rows[] = {
      {SDL_SCANCODE_RETURN, 0x0010, "Enter=START"},  {SDL_SCANCODE_ESCAPE, 0x0020, "Esc=BACK"},
      {SDL_SCANCODE_BACKSPACE, 0x0020, "Backspace=BACK"}, {SDL_SCANCODE_UP, 0x0001, "Up"},
      {SDL_SCANCODE_DOWN, 0x0002, "Down"},           {SDL_SCANCODE_LEFT, 0x0004, "Left"},
      {SDL_SCANCODE_RIGHT, 0x0008, "Right"},         {SDL_SCANCODE_Q, 0x0100, "Q=LB"},
      {SDL_SCANCODE_E, 0x0200, "E=RB"},              {SDL_SCANCODE_Z, 0x1000, "Z=A"},
      {SDL_SCANCODE_SPACE, 0x1000, "Space=A"},       {SDL_SCANCODE_X, 0x2000, "X=B"},
      {SDL_SCANCODE_C, 0x4000, "C=X"},               {SDL_SCANCODE_V, 0x8000, "V=Y"},
  };
  uint8_t keys[A::kHarnessKeyCount] = {};
  bool allRows = true;
  for (const Row& r : rows) {
    std::memset(keys, 0, sizeof(keys));
    keys[r.sc] = 1;
    int16_t lx = 7, ly = 9;
    const uint16_t m = A::mapKeysToPad(keys, &lx, &ly);
    if (m != r.mask || lx != 7 || ly != 9) {
      allRows = false;
      std::printf("      row %s: mask 0x%04X (want 0x%04X) lx=%d ly=%d\n", r.name, m, r.mask, lx, ly);
    }
  }
  check(allRows, "M1 each button key maps to exactly its iter-450S pad bit, sticks untouched");
  {
    std::memset(keys, 0, sizeof(keys));
    int16_t lx = 0, ly = 0;
    keys[SDL_SCANCODE_W] = 1; keys[SDL_SCANCODE_A] = 1;
    const uint16_t m = A::mapKeysToPad(keys, &lx, &ly);
    check(m == 0 && ly == 32767 && lx == -32767, "M2 W/A = stick up/left, no button");
    std::memset(keys, 0, sizeof(keys));
    keys[SDL_SCANCODE_S] = 1; keys[SDL_SCANCODE_D] = 1;
    A::mapKeysToPad(keys, &lx, &ly);
    check(ly == -32767 && lx == 32767, "M3 S/D = stick down/right");
  }
  {
    // Every OTHER scancode maps to nothing (F9 included: it is a host hotkey, never pad input).
    bool none = true;
    for (int sc = 0; sc < (int)A::kHarnessKeyCount; ++sc) {
      bool mapped = false;
      for (const Row& r : rows) mapped |= r.sc == sc;
      mapped |= sc == SDL_SCANCODE_W || sc == SDL_SCANCODE_A || sc == SDL_SCANCODE_S ||
                sc == SDL_SCANCODE_D;
      if (mapped) continue;
      std::memset(keys, 0, sizeof(keys));
      keys[sc] = 1;
      int16_t lx = 0, ly = 0;
      if (A::mapKeysToPad(keys, &lx, &ly) != 0 || lx || ly) { none = false; break; }
    }
    check(none, "M4 no unmapped scancode (F9 included) produces pad input");
    check(A::mapKeysToPad(nullptr, nullptr, nullptr) == 0, "M5 null state = 0");
  }

#ifdef _WIN32
  // ---- C: the real channel, by name ----
  // A STALE mapping with a held key and a wrong magic exists BEFORE the game creates its
  // channel (a harness that kept the previous process's handle): init must re-initialise it.
  _putenv_s("SR_INSTANCE_ID", "hatest");
  _putenv_s("SR_WINDOW_BACKGROUND", "");
  _putenv_s("SR_HARNESS_INPUT", "");
  _putenv_s("SR_HARNESS_REAL_INPUT", "");
  const std::string name = A::harnessChannelName("hatest");
  HANDLE stale = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                    A::kHarnessMapBytes, name.c_str());
  uint8_t* sv = stale ? static_cast<uint8_t*>(MapViewOfFile(stale, FILE_MAP_ALL_ACCESS, 0, 0, 0)) : nullptr;
  check(sv != nullptr, "C0 a stale mapping is pre-created with a held key");
  if (sv) { sv[A::kHarnessOffKeys + SDL_SCANCODE_RETURN] = 1; sv[0] = 0xEE; }

  check(eq(A::policy(), true, true, true, false), "C1 policy() reads the environment (automated)");
  A::initAutomation();
  const uint8_t* hk = A::harnessKeys();
  check(hk != nullptr, "C2 initAutomation opened the channel");
  {
    // The background QoS opt-out landed on this process (it has a background policy).
    PROCESS_POWER_THROTTLING_STATE st{};
    st.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
    const bool got = GetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &st, sizeof(st));
    check(got && (st.ControlMask & PROCESS_POWER_THROTTLING_EXECUTION_SPEED) &&
              (st.ControlMask & 0x4) && st.StateMask == 0,
          "C2b background policy opted out of power throttling (HighQoS + timer resolution)");
  }

  HANDLE h = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, name.c_str());
  uint8_t* v = h ? static_cast<uint8_t*>(MapViewOfFile(h, FILE_MAP_ALL_ACCESS, 0, 0, 0)) : nullptr;
  check(v != nullptr, "C3 the harness opens it by name (OpenFileMapping)");
  if (v && hk) {
    uint32_t magic = 0, ver = 0, pid = 0;
    std::memcpy(&magic, v + A::kHarnessOffMagic, 4);
    std::memcpy(&ver, v + A::kHarnessOffVersion, 4);
    std::memcpy(&pid, v + A::kHarnessOffPid, 4);
    check(magic == A::kHarnessMagic && ver == A::kHarnessVersion && pid == GetCurrentProcessId(),
          "C4 magic SRHI, version 1, this PID");
    check(v[A::kHarnessOffKeys + SDL_SCANCODE_RETURN] == 0,
          "C5 the stale mapping's held Enter was cleared at init");
    v[A::kHarnessOffKeys + SDL_SCANCODE_DOWN] = 1;
    int16_t lx = 0, ly = 0;
    check(A::mapKeysToPad(hk, &lx, &ly) == 0x0002, "C6 a harness byte is the game's key (DOWN -> 0x0002)");
    A::harnessNoteMerge(0x0002);
    A::harnessNoteMerge(0x0002);
    uint32_t cnt = 0, last = 0;
    std::memcpy(&cnt, v + A::kHarnessOffPollCount, 4);
    std::memcpy(&last, v + A::kHarnessOffLastMask, 4);
    check(cnt == 2 && last == 0x0002, "C7 harnessNoteMerge publishes poll count + last mask");
    v[A::kHarnessOffKeys + SDL_SCANCODE_DOWN] = 0;
    check(A::mapKeysToPad(hk, &lx, &ly) == 0, "C8 released byte = no input");
  }
  if (v) UnmapViewOfFile(v);
  if (h) CloseHandle(h);
  if (sv) UnmapViewOfFile(sv);
  if (stale) CloseHandle(stale);
#endif

  std::printf("host-automation-test: %d passed, %d failed\n", g_pass, g_fail);
  return g_fail;
}
