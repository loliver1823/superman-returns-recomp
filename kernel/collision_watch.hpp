// kernel/collision_watch.hpp - the fault-free read layer of kernel/collision_watch.cpp and the
// entry points its difftest (tools/difftest/collision_watch_test.cpp, --collision-watch-test)
// exercises. Kernel-only: NOT included by guest TUs (guest_context.hpp stays untouched).
#pragma once
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace sr::kernel::collwatch {

// Every guest read the watch makes goes through Mem: a read is attempted only when [va, va+n)
// lies inside [kLo, kTop); anything else returns 0 / NaN WITHOUT touching memory.
constexpr uint32_t kLo = 0x00010000u;
constexpr uint32_t kTop = 0xA0000000u;   // aliasGuestVA(va) == va below this

// True iff [va, va+n) is inside the window every read is confined to. Pure arithmetic.
inline bool inWindow(uint32_t va, uint32_t n) { return va >= kLo && va < kTop && n <= kTop - va; }

struct Mem {
  uint8_t* base;
  uint32_t u32(uint32_t va) const {
    if (!inWindow(va, 4)) return 0;
    uint32_t v;
    std::memcpy(&v, base + va, 4);
    return __builtin_bswap32(v);
  }
  uint8_t u8(uint32_t va) const { return inWindow(va, 1) ? base[va] : 0; }
  float f32(uint32_t va) const {
    uint32_t u = u32(va);
    float f;
    std::memcpy(&f, &u, 4);
    return f;
  }
  void vec3(uint32_t va, float o[3]) const {
    if (!inWindow(va, 12)) { o[0] = o[1] = o[2] = NAN; return; }
    for (int i = 0; i < 3; ++i) o[i] = f32(va + 4 * i);
  }
};

// One-time precondition for enabling: flat guest model and [base, base+kTop) committed,
// readable, no guard page (VirtualQuery walk). `why` gets a one-line reason either way.
bool flatModelReadable(uint8_t* base, char* why, size_t whyN);

// [COLL_BODY] de-dup: true the first time (body, translation) is seen.
bool firstSighting(uint32_t body, const float p[3]);

// Per-second line budget.
struct Budget;
bool budgetForTest(uint32_t perSec, int calls);   // returns true iff exactly perSec of `calls` pass

}  // namespace sr::kernel::collwatch
