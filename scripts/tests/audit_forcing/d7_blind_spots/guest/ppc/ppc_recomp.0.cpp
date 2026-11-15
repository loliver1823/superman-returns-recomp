// D7 fixture CONTROL: guest/ppc/** is GENERATED. Its hand edits are audited by
// scan_generated (and splice_bodies.json), never by the function classifier, so
// this write must NOT appear as a site (it carries no EXPECT marker on purpose).
#include <cstdint>
namespace sr { namespace kernel { void writeGuestBE32(uint32_t, uint32_t); } }
void generatedLooking(uint32_t va) {
  sr::kernel::writeGuestBE32(va, 1u);
}
