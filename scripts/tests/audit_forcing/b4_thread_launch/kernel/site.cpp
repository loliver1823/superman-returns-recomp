// B4 fixture: a thread entry launched only inside if (forcingAllowed()) (or
// after a master early-return) is master-gated even when its own body names an
// own gate (fe_flow_watch.cpp feSmKickThread, rtam_service.cpp rtamIniMain).
#include <cstdint>
#include <cstdlib>
#include <thread>
#include "guest_caller.hpp"

bool natInitOn() {
  const char* e = std::getenv("SR_FIXTURE_B4_NAT");
  return !e || e[0] != '0';
}

void kickThread(uint8_t* gb) {
  if (natInitOn()) return;
  sr::kernel::GuestCaller gc("KICK");  // EXPECT: MASTER
}

void earlyThread() {
  if (!natInitOn()) sr::kernel::writeGuestBE32(0x10u, 1u);  // EXPECT: MASTER
}

// Control: launched from the else branch of the master check -> not gated.
void leakThread(uint8_t* gb) {
  if (!natInitOn()) return;
  sr::kernel::GuestCaller gc("LEAK");  // EXPECT: OWNGATE_ON
}

struct Starter {
  Starter() {
    if (sr::kernel::forcingAllowed()) {
      std::thread(kickThread, nullptr).detach();
    } else {
      std::thread(leakThread, nullptr).detach();
    }
  }
} s_starter;

void startEarly() {
  if (!sr::kernel::forcingAllowed()) return;
  std::thread(earlyThread).detach();
}
