// iter-466 tick: guest PGO training builds only (CMake SR_GUEST_PGO=gen compiles this into the
// guest lib; the exe keeps it via /INCLUDE:sr_pgo_dump_anchor). A roll ends by killing the game,
// so the profile runtime's atexit dump never runs: a detached thread rewrites the cumulative
// profile every SR_PGO_DUMP_SECS seconds (default 30) to LLVM_PROFILE_FILE.
#include <chrono>
#include <cstdlib>
#include <thread>

extern "C" int __llvm_profile_write_file(void);
extern "C" int sr_pgo_dump_anchor = 0;

namespace {
struct PgoPeriodicDump {
  PgoPeriodicDump() {
    std::thread([] {
      const char* e = std::getenv("SR_PGO_DUMP_SECS");
      const int secs = (e && std::atoi(e) > 0) ? std::atoi(e) : 30;
      for (;;) {
        std::this_thread::sleep_for(std::chrono::seconds(secs));
        __llvm_profile_write_file();
      }
    }).detach();
  }
} g_pgoPeriodicDump;
}  // namespace
