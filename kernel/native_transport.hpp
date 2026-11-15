#pragma once

#include <cstdint>
struct PPCContext;

namespace sr::native::transport {
// Development cutover gate. The CPU library's software list is retained while
// its pump is converted; these entries do not parse GPU commands or registers.
bool gateOn();
bool appendJumpSegment(PPCContext& ctx, uint8_t* base);
bool endSync(PPCContext& ctx, uint8_t* base);
void finalLine(const char* why);

// The census counters (the [NATIVE_TRANSPORT] line's fields). abOffAppend / abOffEnd count the calls an
// SR_AB_TOGGLE=SR_NATIVE_TRANSPORT OFF window sent to the SDK body whole: neither native calls nor declines.
// exclusion: entries declined because a page-protecting diagnostic is requested or declared (kernel/guest_protect.hpp);
// foreignRange: admissions refused because a range touches a declared foreign-protected range (each also counted in
// its entry's positional reason: appendInput, tailInput, endState or endMarker); retired: entries declined because the
// guest arena was retired for its release (kernel/guest_protect.hpp ArenaLease).
struct Counters {
  uint64_t append, chain, direct, park, resume, decline, abOffAppend, abOffEnd;
  uint64_t exclusion, foreignRange, appendInput, tailInput, endState, endMarker, retired;
};
Counters counters();
}
