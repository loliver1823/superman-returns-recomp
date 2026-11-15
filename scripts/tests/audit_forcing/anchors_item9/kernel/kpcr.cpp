// Allowlist-anchor fixture (round-3 verifier item 9). The file names match the
// real allowlist's file substrings on purpose, so the REAL ALLOWLIST decides.
// Round 3's `fn:writeU8` covered a GENERIC guest-byte writer: a NEW caller that
// forces through it would read ALLOW. Now writeU8 takes its callers' class.
#include <cstdint>
namespace sr { namespace kernel { uint8_t* guestVAToHost(uint32_t); } }
using sr::kernel::guestVAToHost;
void writeU8(uint32_t va, uint8_t v) {
  if (uint8_t* p = guestVAToHost(va)) *p = v;  // EXPECT: UNGATED
}
// a new, ungated, non-platform caller
void forceSomething(uint32_t va) { writeU8(va, 1u); }
int SDL_main(int, char**) { forceSomething(0x10u); return 0; }
