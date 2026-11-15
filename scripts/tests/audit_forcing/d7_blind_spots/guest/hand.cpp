// D7 fixture: hand-written guest/*.cpp is runtime code and is classified like
// kernel/ (guest/guest_harness.cpp, guest/ppc_import_glue.cpp).
#include <cstdint>
#include <cstring>
namespace sr { namespace kernel { void writeGuestBE32(uint32_t, uint32_t); } }
struct PPCContext { struct { uint32_t u32; } r3; };

void guestHelperWrite(uint32_t va) {
  sr::kernel::writeGuestBE32(va, 1u);  // EXPECT: UNGATED
}

void harnessInit(uint8_t* base) {
  std::memset(base + 0x1000u, 0, 0x100u);  // EXPECT: UNGATED
}

void sub_82000080(PPCContext& ctx, uint8_t* base) {
  guestHelperWrite(ctx.r3.u32);
  harnessInit(base);
}
