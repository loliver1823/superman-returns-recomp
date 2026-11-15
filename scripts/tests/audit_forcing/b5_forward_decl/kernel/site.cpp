// B5 fixture: a forward declaration above a function became that function's
// name (stubs.cpp: `void notifyFMWorkerCreated(...);` named stub_ExCreateThread),
// so the export check missed the export's own out-param writes.
#include <cstdint>
#include <cstdlib>
namespace sr { namespace kernel { void writeGuestBE32(uint32_t, uint32_t); } }
struct ExportRegistry { void registerExport(const char*, uint32_t (*)(uint32_t, uint32_t, uint32_t)); };

bool obOn() {
  const char* e = std::getenv("SR_FIXTURE_B5_OB");
  return !e || e[0] != '0';
}

void notifyWorkerCreated(uint32_t ctx, uint32_t handle);
static uint32_t stub_ExCreateThing(uint32_t r3, uint32_t r4,
                                   uint32_t r5) {
  uint32_t handle = 7;
  if (obOn()) notifyWorkerCreated(r4, handle);
  if (r3) sr::kernel::writeGuestBE32(r3, handle);  // EXPECT: ALLOW fn=stub_ExCreateThing
  if (r5) sr::kernel::writeGuestBE32(r5, handle);  // EXPECT: ALLOW fn=stub_ExCreateThing
  return 0;
}

void registerThings(ExportRegistry& reg) { reg.registerExport("ExCreateThing", stub_ExCreateThing); }

// Control: a one-line function right after a namespace opens keeps its name.
namespace {
inline void wr(uint32_t va) { sr::kernel::writeGuestBE32(va, 1u); }  // EXPECT: OWNGATE fn=wr
}  // namespace
