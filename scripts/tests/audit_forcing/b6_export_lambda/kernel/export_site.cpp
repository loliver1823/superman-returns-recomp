// B6 fixture: a lambda passed straight to registerExport("X", ...) IS export X.
// The old audit used the registering function's body (registerStubs), where an
// unrelated txc::enabled() call made every lambda write DEFAULT-ON.
#include <cstdint>
namespace sr { namespace kernel {
void writeGuestBE32(uint32_t, uint32_t);
namespace txc { bool enabled(); }
struct ExportRegistry {
  void registerExport(const char*, uint32_t (*)(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t));
};

void registerStubs(ExportRegistry& reg) {
  reg.registerExport("NtResumeThing",
                     +[](uint32_t r3, uint32_t r4, uint32_t, uint32_t, uint32_t, uint32_t) -> uint32_t {
    if (r4) writeGuestBE32(r4, 1u);  // EXPECT: ALLOW
    return r3 ? 0u : 1u;
  });
  // Control: a write in the registering function itself keeps its gate.
  if (sr::kernel::txc::enabled()) {
    writeGuestBE32(0x10u, 2u);  // EXPECT: OWNGATE_ON
  }
}
} }
