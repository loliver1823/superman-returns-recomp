#pragma once
// Dependency double for the standalone witness test, never a game include path.
#include <cstdint>
struct PPCRegister { union { uint64_t u64; int64_t s64; }; };
struct PPCContext { PPCRegister r3{}, r4{}, r5{}, r6{}, r10{}; };
#define PPC_FUNC(name) extern "C" void name(PPCContext& ctx, uint8_t* base)
PPC_FUNC(sub_82117018);
