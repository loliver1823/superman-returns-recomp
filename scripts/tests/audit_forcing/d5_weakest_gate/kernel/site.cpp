// D5 fixture: with several own gates, the WEAKEST one that can let the write
// run decides. Round 1's body_gate returned the first gate in the text, so a
// default-OFF logging gate ahead of the default-ON gate on the write (verifier
// P7) made a shipping write read opt-in.
#include <cstdint>
#include <cstdlib>
namespace sr { namespace kernel { void writeGuestBE32(uint32_t, uint32_t); } }
struct PPCContext { struct { uint32_t u32; } r3; };
void logit();

bool traceOn() {                       // default OFF (logging)
  const char* e = std::getenv("SR_FIXTURE_D5_TRACE");
  return e && e[0] == '1';
}

bool fabOn() {                         // default ON
  const char* e = std::getenv("SR_FIXTURE_D5_FAB");
  return !e || e[0] != '0';
}

// P7: the logging gate comes first in the text; the write is under fabOn().
void d5LogFirst(uint32_t va) {
  if (traceOn()) logit();
  if (fabOn()) sr::kernel::writeGuestBE32(va, 7u);  // EXPECT: OWNGATE_ON
}

// UNSAFE-direction case: either gate lets the write run -> the default-ON one.
void d5Either(uint32_t va) {
  if (traceOn() || fabOn()) sr::kernel::writeGuestBE32(va, 8u);  // EXPECT: OWNGATE_ON
}

// Control: both gates required -> the write needs the opt-in one.
void d5Both(uint32_t va) {
  if (traceOn() && fabOn()) sr::kernel::writeGuestBE32(va, 9u);  // EXPECT: OWNGATE
}

// Caller dominance: a helper that an export calls (runs by default, ALLOW)
// and an opt-in path also calls is ALLOW - it ships - not "opt-in".
struct ExportRegistry { void registerExport(const char*, uint32_t (*)(uint32_t, uint32_t)); };
static void fillOut(uint32_t va) { sr::kernel::writeGuestBE32(va, 1u); }  // EXPECT: ALLOW
static uint32_t stub_XFixFill(uint32_t r3, uint32_t) { fillOut(r3); return 0; }
void registerD5(ExportRegistry& reg) { reg.registerExport("XFixFill", stub_XFixFill); }
void diagPath(uint32_t va) { if (traceOn()) fillOut(va); }

void sub_82000050(PPCContext& ctx, uint8_t* base) {
  diagPath(ctx.r3.u32);
  d5LogFirst(ctx.r3.u32);
  d5Either(ctx.r3.u32);
  d5Both(ctx.r3.u32);
}
