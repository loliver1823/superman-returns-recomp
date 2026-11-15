// B7 fixture: a helper that only exports call is export code
// (stubs.cpp keWriteGuestDispatcherHeader, kthread_objects.cpp
// obReferenceObjectByHandle called from a registerExport lambda).
#include <cstdint>
#include <cstdlib>
#include <thread>
namespace sr { namespace kernel { void writeGuestBE32(uint32_t, uint32_t); } }
struct ExportRegistry {
  void registerExport(const char*, uint32_t (*)(uint32_t, uint32_t));
};

bool hdrOn() {
  const char* e = std::getenv("SR_FIXTURE_B7_HDR");
  return !e || e[0] != '0';
}

static void writeHeader(uint32_t va, uint32_t type) {
  if (!hdrOn()) return;
  sr::kernel::writeGuestBE32(va, type << 24);  // EXPECT: ALLOW
}
static uint32_t stub_KeInitializeThing(uint32_t r3, uint32_t r4) { writeHeader(r3, r4); return 0; }
static uint32_t stub_KeInitializeOther(uint32_t r3, uint32_t) { writeHeader(r3, 5u); return 0; }

// Called from an export lambda (needs B6 to see the lambda as the export).
static uint32_t objLookup(uint32_t h, uint32_t outVA) {
  if (!hdrOn()) return 1;
  sr::kernel::writeGuestBE32(outVA, h);  // EXPECT: ALLOW
  return 0;
}

// Control: one caller is a host thread, not an export -> not export code.
static void sharedHelper(uint32_t va) {
  if (!hdrOn()) return;
  sr::kernel::writeGuestBE32(va, 9u);  // EXPECT: OWNGATE_ON
}
static uint32_t stub_KeUseShared(uint32_t r3, uint32_t) { sharedHelper(r3); return 0; }
void hostPoller() { sharedHelper(0x10u); }
void startPoller() { std::thread(hostPoller).detach(); }

void registerAll(ExportRegistry& reg) {
  reg.registerExport("KeInitializeThing", stub_KeInitializeThing);
  reg.registerExport("KeInitializeOther", stub_KeInitializeOther);
  reg.registerExport("KeUseShared", stub_KeUseShared);
  reg.registerExport("ObLookupThing", +[](uint32_t r3, uint32_t r4) -> uint32_t {
    if (hdrOn()) return objLookup(r3, r4);
    return 0;
  });
}
