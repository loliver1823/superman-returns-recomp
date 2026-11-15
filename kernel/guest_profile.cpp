// iter-461: a sampling profiler for the guest threads (SR_GUEST_PROFILE_MS=<interval>).
//
// The 20 s [THREAD_SAMPLER] names a hang; it cannot say where a frame's time
// goes. This samples every registered guest thread (g_debugThreadRegistry)
// at the given interval: the link register (the innermost call site) and the
// PPC back-chain above it (saved LR at [chain - 8], next chain at [chain] -
// the walk tools/frida_mcp.py does), and accumulates two histograms per
// thread: SELF (lr) and INCLUSIVE (every frame of the walk, once per sample).
// Every 30 s and at exit (std::exit or std::quick_exit) the histograms go to archive/guest_profile/<pid>.txt
// (raw VAs; tools/guest_profile.py symbolizes them with the function mapping).
//
// Fault-free by construction (the harness VEH makes any host AV fatal): the
// context fields are read through hostRangeReadableCached, guest words through
// ReadProcessMemory on our own process (a miss returns false, never faults).

#include "guest_context.hpp"
#include "../app/log.hpp"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace sr::kernel {

extern "C" __declspec(dllexport) GuestThreadSlot g_debugThreadRegistry[64];
// (g_debugGuestBase: the header's global declaration.)

namespace {

struct TidProf {
  std::string name;
  uint64_t samples = 0;
  std::unordered_map<uint32_t, uint32_t> self;       // lr -> count
  std::unordered_map<uint32_t, uint32_t> inclusive;  // any frame's lr -> count (once per sample)
};

std::unordered_map<uint32_t, TidProf> g_prof;  // by host tid (profiler thread only)
std::atomic<bool> g_profStop{false};
uint64_t g_profSamples = 0, g_profWalks = 0, g_profFrames = 0;
int g_profDumps = 0;
DWORD g_profPid = 0;

bool rdGuest32(uint32_t va, uint32_t& out) {
  if (!g_debugGuestBase || va < 0x10000u) return false;
  uint8_t b[4];
  SIZE_T got = 0;
  if (!ReadProcessMemory(GetCurrentProcess(), g_debugGuestBase + va, b, 4, &got) || got != 4)
    return false;
  out = (uint32_t(b[0]) << 24) | (uint32_t(b[1]) << 16) | (uint32_t(b[2]) << 8) | uint32_t(b[3]);
  return true;
}

inline bool okCode(uint32_t v) { return v >= 0x82000000u && v < 0x82C00000u; }
inline bool okSp(uint32_t v) { return v >= 0x10000u && v < 0xC0000000u; }

// The back-chain walk (frida_mcp.py guestStack): frames above `sp`, at most maxFrames.
uint32_t walk(uint32_t sp, uint32_t* out, uint32_t maxFrames) {
  uint32_t n = 0;
  uint32_t bc = 0, w = 0;
  if (rdGuest32(sp, w) && okSp(w) && w > sp) {
    uint32_t lr = 0;
    if (rdGuest32(w - 8, lr) && okCode(lr)) bc = w;
  }
  if (!bc) {
    for (uint32_t off = 0; off <= 0x60 && !bc; off += 4) {
      uint32_t cand = 0, lr = 0;
      if (rdGuest32(sp + off, cand) && okSp(cand) && cand > sp + off && rdGuest32(cand - 8, lr) &&
          okCode(lr))
        bc = cand;
    }
  }
  for (uint32_t i = 0; i < maxFrames && bc; i++) {
    uint32_t savedLr = 0;
    if (!rdGuest32(bc - 8, savedLr)) break;
    if (okCode(savedLr)) out[n++] = savedLr;
    uint32_t next = 0;
    if (!rdGuest32(bc, next) || next <= bc) break;
    bc = next;
  }
  return n;
}

// One file per 30 s INTERVAL (prof_<pid>_<n>.txt; the histograms reset after
// each) so an era - the intro, the title city, the street - reads on its own;
// the run's timeline maps to n by the [GUEST_PROFILE] lines in the log.
void dump(const char* why) {
  char dir[64];
  std::snprintf(dir, sizeof(dir), "archive/guest_profile");
  CreateDirectoryA("archive", nullptr);
  CreateDirectoryA(dir, nullptr);
  char path[128];
  std::snprintf(path, sizeof(path), "%s/prof_%lu_%02d.txt", dir, (unsigned long)g_profPid,
                g_profDumps + 1);
  FILE* f = std::fopen(path, "w");
  if (!f) return;
  std::fprintf(f, "# guest profile pid=%lu interval=%d (%s) samples=%llu walks=%llu frames=%llu\n",
               (unsigned long)g_profPid, ++g_profDumps, why, (unsigned long long)g_profSamples,
               (unsigned long long)g_profWalks, (unsigned long long)g_profFrames);
  SR_LOG_BOOT(sr::LogSeverity::Info, "[GUEST_PROFILE] interval %d written (%s): %llu samples",
              g_profDumps, why, (unsigned long long)g_profSamples);
  std::vector<std::pair<uint32_t, const TidProf*>> tids;
  for (const auto& kv : g_prof) tids.push_back({kv.first, &kv.second});
  std::sort(tids.begin(), tids.end(),
            [](const auto& a, const auto& b) { return a.second->samples > b.second->samples; });
  for (const auto& t : tids) {
    const TidProf& p = *t.second;
    std::fprintf(f, "tid %u name=%s samples=%llu distinct_self=%zu\n", t.first,
                 p.name.empty() ? "-" : p.name.c_str(), (unsigned long long)p.samples,
                 p.self.size());
    auto top = [&](const char* tag, const std::unordered_map<uint32_t, uint32_t>& m, size_t k) {
      std::vector<std::pair<uint32_t, uint32_t>> v(m.begin(), m.end());
      std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
      for (size_t i = 0; i < v.size() && i < k; i++)
        std::fprintf(f, "%s %u %08X %u\n", tag, t.first, v[i].first, v[i].second);
    };
    top("self", p.self, 80);
    top("incl", p.inclusive, 120);
  }
  std::fclose(f);
  // The next interval starts over (thread names stay).
  for (auto& kv : g_prof) {
    kv.second.samples = 0;
    kv.second.self.clear();
    kv.second.inclusive.clear();
  }
  g_profSamples = g_profWalks = g_profFrames = 0;
}

void profilerMain(int intervalMs) {
  g_profPid = GetCurrentProcessId();
  SR_LOG_BOOT(sr::LogSeverity::Info, "[GUEST_PROFILE] sampling every %d ms -> archive/guest_profile/prof_%lu.txt",
              intervalMs, (unsigned long)g_profPid);
  auto lastDump = std::chrono::steady_clock::now();
  uint32_t frames[32];
  std::vector<uint32_t> seen;
  while (!g_profStop.load(std::memory_order_relaxed)) {
    std::this_thread::sleep_for(std::chrono::milliseconds(intervalMs));
    for (int i = 0; i < 64; ++i) {
      const GuestThreadSlot& s = g_debugThreadRegistry[i];
      void* ctx = s.ctx;
      if (!s.tid || !ctx || !s.lrOff) continue;
      const uint32_t maxOff = std::max(s.lrOff, s.r1Off);
      if (!hostRangeReadableCached(ctx, size_t(maxOff) + 4)) continue;
      const uint32_t lr = *reinterpret_cast<volatile uint32_t*>(static_cast<char*>(ctx) + s.lrOff);
      const uint32_t sp = s.r1Off ? *reinterpret_cast<volatile uint32_t*>(static_cast<char*>(ctx) + s.r1Off) : 0;
      if (!okCode(lr)) continue;  // a host thread parked in host code (lr 0) is not guest time
      TidProf& p = g_prof[s.tid];
      if (p.name.empty() && s.name[0]) p.name = s.name;
      p.samples++;
      g_profSamples++;
      p.self[lr]++;
      seen.clear();
      seen.push_back(lr);
      if (sp) {
        const uint32_t n = walk(sp, frames, 24);
        g_profWalks++;
        g_profFrames += n;
        for (uint32_t k = 0; k < n; k++) {
          if (std::find(seen.begin(), seen.end(), frames[k]) != seen.end()) continue;
          seen.push_back(frames[k]);
        }
      }
      for (uint32_t va : seen) p.inclusive[va]++;
    }
    const auto now = std::chrono::steady_clock::now();
    if (now - lastDump >= std::chrono::seconds(30)) {
      dump("periodic");
      lastDump = now;
    }
  }
  dump("stop");
}

}  // namespace

namespace {
void finalDump() {
  static std::atomic<bool> s_dumped{false};
  if (s_dumped.exchange(true)) return;
  g_profStop.store(true, std::memory_order_relaxed);
  dump("exit");
}
}  // namespace

void startGuestProfiler() {
  static std::atomic<bool> s_started{false};
  if (s_started.exchange(true)) return;
  const char* e = std::getenv("SR_GUEST_PROFILE_MS");
  if (!e || !e[0]) return;
  const int ms = std::max(1, std::atoi(e));
  // The final dump runs on either exit route: std::exit (CRT teardown) runs atexit
  // handlers, app/main.cpp hostExitNow (SR_FAST_EXIT, the window-close / SR_Quit path)
  // leaves through std::quick_exit, which runs only at_quick_exit handlers.
  std::atexit(finalDump);
  std::at_quick_exit(finalDump);
  std::thread([ms] { profilerMain(ms); }).detach();
}

}  // namespace sr::kernel
