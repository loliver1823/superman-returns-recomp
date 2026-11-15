// D14 fixture (round-3 verifier probe tree v3/p4 Q4 / Q5): the export rule
// ("an export fills its own out-params") turned code that SAYS it is forcing
// into ALLOW.
#include <cstdint>
#include <cstdlib>
namespace sr { namespace kernel { bool forcingAllowed(); void writeGuestBE32(uint32_t, uint32_t); } }
struct ExportRegistry { void registerExport(const char*, uint32_t (*)(uint32_t, uint32_t)); };

static bool autoRel() {
  const char* e = std::getenv("SR_FIXTURE_D14_AUTOREL");
  return !e || e[0] != '0';
}
// UNSAFE-direction case Q4: an OR-bypass of the master gate inside an export.
static uint32_t stub_XD14Or(uint32_t r3, uint32_t) {
  if (autoRel() || sr::kernel::forcingAllowed()) sr::kernel::writeGuestBE32(r3, 4u);  // EXPECT: OWNGATE_ON
  return 0;
}
// UNSAFE-direction case Q5: a helper an export calls ONLY under the master gate
// (the VdSwap fake-completion shape one level down).
static void fakeCompletion(uint32_t va) { sr::kernel::writeGuestBE32(va, 5u); }  // EXPECT: MASTER
static uint32_t stub_XD14Fake(uint32_t r3, uint32_t) {
  if (sr::kernel::forcingAllowed()) fakeCompletion(0x7FC80714u);
  return r3;
}
// Controls: the export's own out-param write, and a helper it calls by
// default, stay ALLOW.
static void fillOut(uint32_t va, uint32_t v) { sr::kernel::writeGuestBE32(va, v); }  // EXPECT: ALLOW
static uint32_t stub_XD14Out(uint32_t r3, uint32_t r4) {
  if (r4) sr::kernel::writeGuestBE32(r4, r3);  // EXPECT: ALLOW
  fillOut(r3, 1u);
  return 0;
}
void registerD14(ExportRegistry& reg) {
  reg.registerExport("XD14Or", stub_XD14Or);
  reg.registerExport("XD14Fake", stub_XD14Fake);
  reg.registerExport("XD14Out", stub_XD14Out);
}
