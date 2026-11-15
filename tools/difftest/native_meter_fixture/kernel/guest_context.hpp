#pragma once
#include <cstdint>
namespace sr::kernel {
uint32_t readGuestU32BE(uint32_t);
void writeGuestBE32(uint32_t, uint32_t);
}
