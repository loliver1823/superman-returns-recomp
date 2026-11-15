// D10 fixture: a CALLER's kill switch is not a gate on the callee. app/main.cpp
// SDL_main returns early under SR_H7_ENTRYWATCH_TEST=1 (a test mode, default
// OFF) before it boots the harness; read as `requires !testMode` = "default-ON
// own gate", that laundered every ungated write reached from main (the
// 0x8249AAC8 allocator patch in initKernelDataStructures) into DEFAULT-ON.
#include <cstdint>
#include <cstdlib>
namespace sr { namespace kernel { void writeGuestBE32(uint32_t, uint32_t); } }

bool platformOn() {                    // a real default-ON A/B switch
  const char* e = std::getenv("SR_FIXTURE_D10_PLATFORM");
  return !e || e[0] != '0';
}

// UNSAFE-direction case: reached from main only past the test-mode return.
void bootWrites() {
  sr::kernel::writeGuestBE32(0x8249AAC8u, 1u);  // EXPECT: UNGATED
}

// Control: a caller that calls it under a POSITIVE default-ON switch gates it.
void platformWrites() {
  sr::kernel::writeGuestBE32(0x82000948u, 2u);  // EXPECT: OWNGATE_ON
}

// Control: the kill switch in the site's OWN function is its own A/B switch.
void ownKill() {
  if (!std::getenv("SR_FIXTURE_D10_OWNKILL")) sr::kernel::writeGuestBE32(0x10u, 3u);  // EXPECT: OWNGATE_ON
}

int main() {
  const bool testMode = [] {
    const char* e = std::getenv("SR_FIXTURE_D10_TEST");
    return e && e[0] == '1';
  }();
  if (testMode) {
    return 0;
  }
  bootWrites();
  if (platformOn()) platformWrites();
  ownKill();
  return 0;
}
