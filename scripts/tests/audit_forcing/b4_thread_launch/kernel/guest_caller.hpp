// B4 fixture: declarations only (the audit scans .cpp/.cc, not headers).
#pragma once
#include <cstdint>
namespace sr { namespace kernel {
bool forcingAllowed();
void writeGuestBE32(uint32_t, uint32_t);
struct GuestCaller { explicit GuestCaller(const char*); };
} }
