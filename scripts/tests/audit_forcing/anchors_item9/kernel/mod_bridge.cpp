// Allowlist-anchor fixture (item 9): round 3 anchored the mod-bridge reply on the
// COMMENT `// back chain`, so any write within +-5 lines of that comment read
// ALLOW. Now each reply write is listed by its exact code.
#include <cstdint>
#include <cstring>
namespace sr { namespace kernel { uint8_t* guestVAToHost(uint32_t); void writeGuestBE32(uint32_t, uint32_t); } }
using namespace sr::kernel;
void modBridgeLike(uint32_t frame, uint32_t savedR1) {
  writeGuestBE32(frame + 0x40u, 0xDEADu);  /* EXPECT: UNGATED */  // back chain
  writeGuestBE32(frame, savedR1);  // EXPECT: ALLOW
  const uint32_t pathVA = frame + 0xB0u;
  std::memcpy(guestVAToHost(pathVA), "FlowMgr", 8);  // EXPECT: ALLOW
}
int SDL_main(int, char**) { modBridgeLike(0x7000u, 0x7100u); return 0; }
