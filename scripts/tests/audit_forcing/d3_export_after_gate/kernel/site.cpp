// D3 fixture: the export rule ("an export fills its own out-params") ran BEFORE
// the gate check, so master-gated forcing inside an export read ALLOW (the real
// stubs.cpp VdSwap RPTR:=WPTR fake completion did), and a thread an export
// STARTS was treated as the export's own call (verifier P4 / P6).
#include <cstdint>
#include <cstdlib>
#include <thread>
namespace sr { namespace kernel { bool forcingAllowed(); void writeGuestBE32(uint32_t, uint32_t); } }
struct ExportRegistry { void registerExport(const char*, uint32_t (*)(uint32_t, uint32_t)); };

bool diagOn() {
  const char* e = std::getenv("SR_FIXTURE_D3_DIAG");
  return e && e[0] == '1';
}

// UNSAFE-direction case P4: a helper run only by a thread the export starts.
static void bgFab(uint32_t va) { sr::kernel::writeGuestBE32(va, 1u); }  // EXPECT: UNGATED
static uint32_t stub_XFixSpawn(uint32_t r3, uint32_t) {
  std::thread([r3] { for (;;) bgFab(r3); }).detach();
  return 0;
}

// The VdSwap shape: out-param write + master-gated fake completion.
static uint32_t stub_VdSwapFix(uint32_t r3, uint32_t r4) {
  if (r4) sr::kernel::writeGuestBE32(r4, 1u);  // EXPECT: ALLOW
  if (sr::kernel::forcingAllowed()) {
    sr::kernel::writeGuestBE32(0x7FC80714u, r3);  // EXPECT: MASTER
  }
  // an opt-in diagnostic inside an export is still opt-in, not ALLOW
  if (diagOn()) sr::kernel::writeGuestBE32(r3, 7u);  // EXPECT: OWNGATE
  return 0;
}

// A thread ENTRY an export starts under its own default-ON switch
// (stub_VdSetGraphicsInterruptCallback -> vdVblankThread under SR_VD_ISR): the
// entry takes the launch's gate, never the export's ALLOW.
bool isrOn() {
  const char* e = std::getenv("SR_FIXTURE_D3_ISR");
  return !e || e[0] != '0';
}
static void vblankThread() { sr::kernel::writeGuestBE32(0x20u, 8u); }  // EXPECT: OWNGATE_ON
static uint32_t stub_VdSetCbFix(uint32_t r3, uint32_t) {
  if (isrOn()) std::thread(vblankThread).detach();
  return r3;
}

void registerFix(ExportRegistry& reg) {
  reg.registerExport("VdSetCbFix", stub_VdSetCbFix);
  reg.registerExport("XFixSpawn", stub_XFixSpawn);
  reg.registerExport("VdSwapFix", stub_VdSwapFix);
  // P5: master-gated fabrication inside an export lambda.
  reg.registerExport("XFixLam", +[](uint32_t r3, uint32_t) -> uint32_t {
    if (sr::kernel::forcingAllowed()) sr::kernel::writeGuestBE32(r3, 5u);  // EXPECT: MASTER
    return 0;
  });
  // P6 UNSAFE-direction case: a thread spawned inside an export lambda.
  reg.registerExport("XFixLamThread", +[](uint32_t r3, uint32_t) -> uint32_t {
    std::thread([r3] { for (;;) sr::kernel::writeGuestBE32(r3, 6u); }).detach();  // EXPECT: UNGATED
    return 0;
  });
  // Control: the export's own out-param write stays ALLOW.
  reg.registerExport("XFixOut", +[](uint32_t r3, uint32_t r4) -> uint32_t {
    if (r4) sr::kernel::writeGuestBE32(r4, r3);  // EXPECT: ALLOW
    return 0;
  });
}
