// iter-464 bgruns: automated runs never take the foreground - see host_automation.hpp.
#include "host_automation.hpp"
#include "sr_oracle.hpp"
#include "../app/log.hpp"

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <SDL3/SDL.h>
#ifdef _WIN32
#include <windows.h>
#endif

namespace sr::kernel::automation {

namespace {
// nullptr / "" = unset; otherwise '0' = off, anything else = on.
int envTri(const char* v) {
  if (!v || !*v) return -1;
  return v[0] == '0' ? 0 : 1;
}

uint8_t* g_map = nullptr;   // the mapped view (harnessInput only)

#ifdef _WIN32
#ifndef PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION
#define PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION 0x4
#endif
// A background window must not mean a slower game. Windows 11 stops honouring a process's
// timer-resolution request (SDL asks for 1 ms; the game's schedulers sleep in 1 ms steps) once
// all its windows are occluded or minimized, and may run a background process at EcoQoS. An
// occluded roll would then silently run with 15.6 ms sleeps. Opting out (ControlMask set,
// StateMask clear) keeps the foreground behaviour: HighQoS, timer resolution honoured.
// SR_BG_HIGH_QOS=0 skips it (A/B).
const char* applyBackgroundQos() {
  const char* e = std::getenv("SR_BG_HIGH_QOS");
  if (e && e[0] == '0') return "os-default (SR_BG_HIGH_QOS=0)";
  PROCESS_POWER_THROTTLING_STATE st{};
  st.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
  st.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED |
                   PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION;
  st.StateMask = 0;
  if (SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &st, sizeof(st)))
    return "high (no EcoQoS; timer resolution honoured while occluded)";
  return "SET-FAILED";
}

#if SR_ORACLE
// SR_BG_PROBE=1 (observe-only, default OFF): a host thread that, every 10 s for 3 minutes,
// times 20 x Sleep(1) and notes whether our window is the foreground one. A throttled
// (occluded, timer resolution ignored) process sleeps ~15.6 ms; an honoured 1 ms request
// sleeps ~1-2 ms. Touches no guest state.
// C1 (kernel/sr_oracle.hpp): a development experiment, compiled out under SR_ORACLE=0 (SR_BG_PROBE then reads as
// unset). The harness input channel below is not: it is the test instrument the release gates drive.
void startBackgroundProbe() {
  const char* e = std::getenv("SR_BG_PROBE");
  if (!e || e[0] != '1') return;
  std::thread([] {
    LARGE_INTEGER f, a, b;
    QueryPerformanceFrequency(&f);
    for (int t = 10; t <= 180; t += 10) {
      Sleep(10000);
      double sum = 0, mx = 0;
      for (int i = 0; i < 20; ++i) {
        QueryPerformanceCounter(&a);
        Sleep(1);
        QueryPerformanceCounter(&b);
        const double ms = double(b.QuadPart - a.QuadPart) * 1000.0 / double(f.QuadPart);
        sum += ms;
        if (ms > mx) mx = ms;
      }
      DWORD fgPid = 0;
      if (HWND fg = GetForegroundWindow()) GetWindowThreadProcessId(fg, &fgPid);
      SR_LOG_BOOT(sr::LogSeverity::Info,
                  "[BG_PROBE] t=%ds sleep1Avg=%.2fms sleep1Max=%.2fms foregroundIsUs=%d",
                  t, sum / 20.0, mx, fgPid == GetCurrentProcessId() ? 1 : 0);
    }
  }).detach();
}
#endif  // SR_ORACLE
#endif
}  // namespace

Policy computePolicy(const char* instanceId, bool traceReplay, const char* windowBackground,
                     const char* harnessInput, const char* realInput) {
  Policy p;
  p.automated = instanceId && *instanceId;
  const int wb = envTri(windowBackground);
  p.windowBackground = wb >= 0 ? wb == 1 : (p.automated || traceReplay);
  const int hi = envTri(harnessInput);
  p.harnessInput = hi >= 0 ? hi == 1 : p.automated;
  const int ri = envTri(realInput);
  p.realInput = ri >= 0 ? ri == 1 : !p.automated;
  return p;
}

bool portFeaturesFor(const char* instanceId, const char* portFeatures) {
  const int pf = envTri(portFeatures);
  if (pf >= 0) return pf == 1;
  return !(instanceId && *instanceId);
}

bool portFeaturesOn() {
  static const bool on = portFeaturesFor(std::getenv("SR_INSTANCE_ID"), std::getenv("SR_PORT_FEATURES"));
  return on;
}

const Policy& policy() {
  static const Policy p = computePolicy(std::getenv("SR_INSTANCE_ID"),
#if SR_ORACLE
                                        envTri(std::getenv("SR_GPU_TRACE_REPLAY")) >= 0 ||
                                            envTri(std::getenv("SR_HLE_TRACE_REPLAY")) >= 0,
#else
                                        // SR_ORACLE=0: the replay modes are compiled out (app/main.cpp), so
                                        // SR_GPU_TRACE_REPLAY / SR_HLE_TRACE_REPLAY read as unset here too.
                                        false,
#endif
                                        std::getenv("SR_WINDOW_BACKGROUND"),
                                        std::getenv("SR_HARNESS_INPUT"),
                                        std::getenv("SR_HARNESS_REAL_INPUT"));
  return p;
}

std::string harnessChannelName(const char* instanceId) {
  std::string id = (instanceId && *instanceId) ? instanceId : "default";
  for (char& c : id) {
    const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                    c == '_' || c == '.' || c == '-';
    if (!ok) c = '_';
  }
  if (id.size() > 64) id.resize(64);
  return "Local\\SR_HARNESS_INPUT_" + id;
}

void initAutomation() {
  static bool s_done = false;
  if (s_done) return;
  s_done = true;
  const Policy& p = policy();
  const std::string name = harnessChannelName(std::getenv("SR_INSTANCE_ID"));
  const char* chan = "off";
  const char* qos = "unchanged (foreground)";
#ifdef _WIN32
  if (p.windowBackground) qos = applyBackgroundQos();
#if SR_ORACLE
  startBackgroundProbe();
#endif
  if (p.harnessInput) {
    // The handle is deliberately never closed: the mapping lives as long as the process.
    HANDLE h = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                  kHarnessMapBytes, name.c_str());
    if (h) {
      void* v = MapViewOfFile(h, FILE_MAP_ALL_ACCESS, 0, 0, kHarnessMapBytes);
      if (v) {
        auto* b = static_cast<uint8_t*>(v);
        // A mapping left open by a previous harness is re-initialised: no stale held key and
        // no stale magic survive into this process. The magic is stamped LAST, so a harness
        // that sees it (and this PID) sees a fully initialised channel.
        std::atomic_thread_fence(std::memory_order_seq_cst);
        std::memset(b, 0, kHarnessMapBytes);
        const uint32_t ver = kHarnessVersion, pid = GetCurrentProcessId(), magic = kHarnessMagic;
        std::memcpy(b + kHarnessOffVersion, &ver, 4);
        std::memcpy(b + kHarnessOffPid, &pid, 4);
        std::atomic_thread_fence(std::memory_order_seq_cst);
        std::memcpy(b + kHarnessOffMagic, &magic, 4);
        std::atomic_thread_fence(std::memory_order_seq_cst);
        g_map = b;
        chan = "open";
      } else {
        CloseHandle(h);
        chan = "MAP-FAILED";
      }
    } else {
      chan = "CREATE-FAILED";
    }
  }
#endif
  SR_LOG_BOOT(p.harnessInput && !g_map ? sr::LogSeverity::Error : sr::LogSeverity::Info,
              "[AUTOMATION] automated=%d window=%s qos=%s harnessInput=%d realInput=%d channel=%s (%s)%s",
              p.automated ? 1 : 0, p.windowBackground ? "background" : "foreground", qos,
              p.harnessInput ? 1 : 0, p.realInput ? 1 : 0, name.c_str(), chan,
              p.automated && !p.realInput
                  ? " - real keyboard/pads IGNORED (SR_HARNESS_REAL_INPUT=1 to allow)"
                  : "");
}

const uint8_t* harnessKeys() { return g_map ? g_map + kHarnessOffKeys : nullptr; }

void harnessNoteMerge(uint16_t mask) {
  if (!g_map) return;
  auto* cnt = reinterpret_cast<volatile uint32_t*>(g_map + kHarnessOffPollCount);
  auto* last = reinterpret_cast<volatile uint32_t*>(g_map + kHarnessOffLastMask);
  *last = mask;
  *cnt = *cnt + 1;   // single writer (the input poll); the harness only reads it
}

uint32_t harnessFlags() {
  if (!g_map) return 0;
  return *reinterpret_cast<volatile uint32_t*>(g_map + kHarnessOffFlags);
}

bool harnessMouse(HarnessMouse* out) {
  if (!g_map || !(harnessFlags() & kHarnessFlagMouse)) return false;
  const volatile int32_t* m = reinterpret_cast<volatile int32_t*>(g_map + kHarnessOffMouse);
  out->dxTotal = m[0];
  out->dyTotal = m[1];
  out->wheelTotal = m[2];
  out->buttons = static_cast<uint32_t>(m[3]);
  return true;
}

void harnessWriteReadback(const void* bytes, uint32_t n) {
  if (!g_map || !bytes) return;
  if (n > kHarnessReadbackBytes) n = kHarnessReadbackBytes;
  std::memcpy(g_map + kHarnessOffReadback, bytes, n);
}

uint16_t mapKeysToPad(const uint8_t* k, int16_t* thumbLX, int16_t* thumbLY, int16_t* thumbRX, int16_t* thumbRY) {
  if (!k) return 0;
  uint16_t b = 0;
  if (k[SDL_SCANCODE_RETURN])    b |= 0x0010;  // START
  if (k[SDL_SCANCODE_ESCAPE] ||
      k[SDL_SCANCODE_BACKSPACE]) b |= 0x0020;  // BACK
  if (k[SDL_SCANCODE_UP])        b |= 0x0001;  // DPAD_UP
  if (k[SDL_SCANCODE_DOWN])      b |= 0x0002;  // DPAD_DOWN
  if (k[SDL_SCANCODE_LEFT])      b |= 0x0004;  // DPAD_LEFT
  if (k[SDL_SCANCODE_RIGHT])     b |= 0x0008;  // DPAD_RIGHT
  if (k[SDL_SCANCODE_Q])         b |= 0x0100;  // LB
  if (k[SDL_SCANCODE_E])         b |= 0x0200;  // RB
  if (k[SDL_SCANCODE_Z] ||
      k[SDL_SCANCODE_SPACE])     b |= 0x1000;  // A
  if (k[SDL_SCANCODE_X])         b |= 0x2000;  // B
  if (k[SDL_SCANCODE_C])         b |= 0x4000;  // X
  if (k[SDL_SCANCODE_V])         b |= 0x8000;  // Y
  if (thumbLY) {
    if (k[SDL_SCANCODE_W])      *thumbLY = 32767;
    else if (k[SDL_SCANCODE_S]) *thumbLY = -32767;
  }
  if (thumbLX) {
    if (k[SDL_SCANCODE_A])      *thumbLX = -32767;
    else if (k[SDL_SCANCODE_D]) *thumbLX = 32767;
  }
  // iter-465 viewer: the right stick for automated rolls (I/K up/down, J/L left/right)
  if (thumbRY) {
    if (k[SDL_SCANCODE_I])      *thumbRY = 32767;
    else if (k[SDL_SCANCODE_K]) *thumbRY = -32767;
  }
  if (thumbRX) {
    if (k[SDL_SCANCODE_J])      *thumbRX = -32767;
    else if (k[SDL_SCANCODE_L]) *thumbRX = 32767;
  }
  return b;
}

}  // namespace sr::kernel::automation
