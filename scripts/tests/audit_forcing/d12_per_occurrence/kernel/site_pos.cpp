// D12 fixture: one line was one site, judged at its FIRST occurrence (round-3
// verifier probe tree v3/p5). Every shape below read a stronger gate than it has.
#include <cstdint>
#include <cstdlib>
namespace sr { namespace kernel { bool forcingAllowed(); void writeGuestBE32(uint32_t, uint32_t); uint8_t* guestVAToHost(uint32_t); } }
void logit();
bool diagOn() {
  const char* e = std::getenv("SR_FIXTURE_D12_DIAG");
  return e && e[0] == '1';
}
// UNSAFE-direction case: reached ungated by the ELSE arm of r5's caller line.
void helper(uint32_t va) { sr::kernel::writeGuestBE32(va, 9u); }  // EXPECT: UNGATED

// the store after the then-block's closing brace, same line
void r1(uint32_t va) {
  uint8_t* h = sr::kernel::guestVAToHost(va);
  if (sr::kernel::forcingAllowed()) {
    logit();
  } else h[0] = 1;  // EXPECT: UNGATED
}
// same, opt-in diagnostic in the then-branch: the store runs by default
void r2(uint32_t va) {
  uint8_t* h = sr::kernel::guestVAToHost(va);
  if (diagOn()) {
    logit();
  } else { h[0] = 1; }  // EXPECT: OWNGATE_ON
}
// two writes on one line, the first master-gated, the second not
void r3(uint32_t a, uint32_t b) {
  if (sr::kernel::forcingAllowed()) sr::kernel::writeGuestBE32(a, 1u); else sr::kernel::writeGuestBE32(b, 2u);  // EXPECT: MASTER,UNGATED
}
// master block then an ungated write, one line
void r4(uint32_t a, uint32_t b) {
  if (sr::kernel::forcingAllowed()) { sr::kernel::writeGuestBE32(a, 1u); } sr::kernel::writeGuestBE32(b, 2u);  // EXPECT: MASTER,UNGATED
}
// caller reference: the first call on the line is master-gated, the second not
void r5(uint32_t a) {
  if (sr::kernel::forcingAllowed()) helper(a); else helper(a + 4);
}
// Control: both occurrences gated stay MASTER (and are two sites).
void r6(uint32_t a, uint32_t b) {
  if (sr::kernel::forcingAllowed()) { sr::kernel::writeGuestBE32(a, 1u); sr::kernel::writeGuestBE32(b, 2u); }  // EXPECT: MASTER,MASTER
}
int SDL_main(int, char**) { r1(0x10); r2(0x14); r3(0x18, 0x1C); r4(0x20, 0x24); r5(0x28); r6(0x30, 0x34); return 0; }
