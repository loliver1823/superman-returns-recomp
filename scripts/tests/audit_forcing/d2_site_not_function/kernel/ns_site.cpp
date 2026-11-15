// D2 fixture CONTROL: an early-exit guard belongs to its own function. In a file
// wrapped in namespaces (kernel/stubs.cpp), the guard in earlier() must not
// gate the write in later() (a statement-splitting bug once leaked function
// bodies into the namespace block and did exactly that).
#include <cstdint>
struct PPCContext { struct { uint32_t u32; } r3; };
namespace sr { namespace kernel {
bool forcingAllowed();
void writeGuestBE32(uint32_t, uint32_t);

void earlier(uint32_t va) {
  if (!forcingAllowed()) return;
  writeGuestBE32(va, 1u);  // EXPECT: MASTER
}

void later(uint32_t va) {
  writeGuestBE32(va, 2u);  // EXPECT: UNGATED
}
} }

void sub_82000024(PPCContext& ctx, uint8_t* base) {
  sr::kernel::earlier(ctx.r3.u32);
  sr::kernel::later(ctx.r3.u32);
}
