// iter-464 perf lane: standalone self-test of the host sampling profiler
// (kernel/host_profile.cpp) and the telemetry thread (kernel/perf_stats.cpp).
//
// Build + run (tools/perf/selftest.ps1 does exactly this):
//   clang++ -std=c++20 -O2 -g -gcodeview -fuse-ld=lld -Wl,/DEBUG -I. tests/perf/host_profile_selftest.cpp
//       kernel/host_profile.cpp kernel/perf_stats.cpp -o build/perf_selftest.exe
//       -ladvapi32 -ldxgi -lpsapi
//   SR_PERF_STATS=1 SR_HOST_PROFILE_HZ=1000 SR_HOST_PROFILE_SECS=1 SR_PERF_DIR=<dir> build/perf_selftest.exe
//   python tools/perf/host_profile.py <dir> --exe build/perf_selftest.exe
//
// Workers: a deep recursive CPU burner (frames must unwind to the thread root), a leaf
// spinner, a waiter (Sleep -> a syscall leaf) and a thread that exits early (handle
// lifetime). Passing = the process survives 4 s of 1 kHz sampling (no fault, no
// deadlock while threads are suspended) and the profile names burnDeep/leafSpin.

#include "kernel/guest_context.hpp"
#include "kernel/perf_stats.hpp"
#include "app/log.hpp"

#include <windows.h>

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <thread>
#include <vector>

// ---- stubs for the symbols the two TUs use from the game -------------------
extern "C" sr::kernel::GuestThreadSlot g_debugThreadRegistry[64] = {};
namespace sr {
namespace kernel {
void getGlobalGuestMemory(uint8_t** outBase, size_t* outSize) {
  *outBase = nullptr;
  *outSize = 0;
}
}  // namespace kernel
void Log::write(LogCategory, LogSeverity, const char* fmt, ...) {
  va_list a;
  va_start(a, fmt);
  std::vfprintf(stderr, fmt, a);
  std::fputc('\n', stderr);
  va_end(a);
}
}  // namespace sr

static std::atomic<bool> g_stop{false};
static volatile uint64_t g_sink = 0;

__declspec(noinline) uint64_t burnDeep(int depth) {
  if (depth > 0) {
    volatile char pad[64];
    pad[0] = char(depth);
    return burnDeep(depth - 1) + pad[0];
  }
  uint64_t x = 0;
  for (int i = 0; i < 200000; ++i) x = x * 6364136223846793005ull + 1442695040888963407ull;
  return x;
}

__declspec(noinline) void leafSpin() {
  for (uint64_t i = 0; i < 2000000; ++i) g_sink = g_sink + i;
}

int main() {
  sr::perf::start();
  std::vector<std::thread> ts;
  ts.emplace_back([] {
    while (!g_stop) g_sink = g_sink + burnDeep(40);
  });
  ts.emplace_back([] {
    while (!g_stop) leafSpin();
  });
  ts.emplace_back([] {
    while (!g_stop) Sleep(5);
  });
  ts.emplace_back([] { Sleep(1500); });  // exits mid-run
  for (int i = 0; i < 40 && !g_stop; ++i) Sleep(100);
  g_stop = true;
  for (auto& t : ts) t.join();
  std::printf("perf selftest: survived; sink=%llu\n", (unsigned long long)g_sink);
  std::exit(0);  // runs the atexit final dumps
}
