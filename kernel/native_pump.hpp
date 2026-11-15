#pragma once

#include <cstdint>
struct PPCContext;

namespace sr::native::pump {
// CPU software-list control flow from retail 82110460. Default OFF while
// generation binding, outer-driver sealing and live cutover remain unfinished.
bool markerGateOn();
bool replayMarkers(PPCContext& ctx, uint8_t* base);
bool driverGateOn();
bool replayDriver(PPCContext& ctx, uint8_t* base);

struct Publication {
  uint32_t record = 0;
  uint32_t state = 0;
};
// Valid only during the actual 820FD218 call. Nesting restores the outer call.
Publication currentPublication();

#if defined(SR_DIFFTEST)
using PublicationObserver = void (*)(Publication);
void setPublicationObserver(PublicationObserver observer);
// Observe after the driver unlocks but before invoking its captured callback.
using DriverCallbackObserver = void (*)(uint32_t handler, uint32_t iteration);
void setDriverCallbackObserver(DriverCallbackObserver observer);
#endif
}
