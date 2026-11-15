#pragma once
// Guest-call ABI double for the standalone host-access fixture only. Production
// sources also compile against the generated ABI in the full release build.
#include <cstdint>
union PPCRegister { uint64_t u64; uint32_t u32; };
union PPCFloat { double f64; };
struct PPCContext {
  PPCRegister r1{}, r3{}, r4{}, r5{}, r6{}, r7{}, r8{}, r9{};
  PPCFloat f1{}, f2{};
  uint64_t lr{};
};
using PPCFunc = void(PPCContext&, uint8_t*);
namespace sr::kernel { extern PPCFunc** g_dispatchTable; }
#define PPC_FUNC(name) extern "C" void name(PPCContext& ctx, uint8_t* base)
inline PPCFunc* pcFixtureLookup(uint32_t address) {
  return address == 0x82001000u && sr::kernel::g_dispatchTable
      ? sr::kernel::g_dispatchTable[0] : nullptr;
}
#define PPC_LOOKUP_FUNC_SAFE(base, address) pcFixtureLookup(address)
PPC_FUNC(sub_821EAA18);
PPC_FUNC(sub_8269A210);
PPC_FUNC(sub_82693028);
PPC_FUNC(sub_825F6AB8);
PPC_FUNC(sub_8268F3E8);
PPC_FUNC(sub_826B05F0);
PPC_FUNC(sub_826B3998);
PPC_FUNC(sub_8229FD48);
PPC_FUNC(sub_8268FB58);
PPC_FUNC(sub_8267F2E8);
