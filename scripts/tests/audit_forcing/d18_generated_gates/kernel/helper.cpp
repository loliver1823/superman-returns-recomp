// D18 fixture: the generated-code hand-edit check (scan_generated). Its gate
// test accepted ANY getenv / forcingAllowed text within 25 lines (a default-ON
// switch, a gate in an unrelated branch), and it saw only sr::kernel::-qualified
// calls, counting only writeGuest* / guestHeapAlloc* / alertableYield as forcing.
// The expectations live in tools/regen/splice_bodies.json (EXPECT-GEN markers).
#include <cstdint>
namespace sr { namespace kernel {
void writeGuestBE32(uint32_t, uint32_t);
void logX(const char*);
// a host function that writes guest memory, called from the splice
void helperWriter(uint32_t va) {
  writeGuestBE32(va, 1u);  // EXPECT: UNGATED
}
} }
