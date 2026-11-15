#pragma once
#include <cstdint>
// Isolate real entry-counter wrappers from native device allocation and guest MMIO.
// This tests wrapper accounting/delegation, not the decoder/register implementation.
namespace sr::kernel::apu {
inline bool fixtureEnabled = false;
inline bool fixtureInit = true;
inline uint64_t fixtureRegisterWrites = 0;
inline bool xmaNewEnabled() { return fixtureEnabled; }
class XmaDevice {
public:
  static XmaDevice& instance() { static XmaDevice value; return value; }
  bool ensureInit() { return fixtureInit; }
  uint32_t contextArrayPA() const { return 0x1FCAA000; }
  uint64_t regfileWrites() const { return fixtureRegisterWrites; }
};
}
