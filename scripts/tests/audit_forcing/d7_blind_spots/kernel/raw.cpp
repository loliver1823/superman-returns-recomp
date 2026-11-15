// D7 fixture: writes that bypass the named helpers. Round 1 saw writeGuest*,
// PPC_STORE_* and memset(guestVAToHost(..)) only: stores through a pointer
// from guestVAToHost / the guest base and memcpy into guest memory were
// invisible (gpu_command_processor.cpp KPCR+268 byte, mod_bridge.cpp name copy,
// guest_context.cpp data-import memcpy).
#include <cstdint>
#include <cstring>
namespace sr { namespace kernel { uint8_t* guestVAToHost(uint32_t); extern uint8_t* g_guestBase; } }
using sr::kernel::g_guestBase;
struct PPCContext { struct { uint32_t u32; } r3; };
void storeBE32(uint8_t*, uint32_t);

void rawWrites(uint32_t va, const uint8_t* src, uint32_t n) {
  uint8_t* h = sr::kernel::guestVAToHost(va);
  h[0] = 1;  // EXPECT: UNGATED
  if (uint8_t* p = sr::kernel::guestVAToHost(va + 4)) *p = 2;  // EXPECT: UNGATED
  std::memcpy(sr::kernel::guestVAToHost(va + 8), src, n);  // EXPECT: UNGATED
  std::memcpy(g_guestBase + 0xD0015000u, src, n);  // EXPECT: UNGATED
  *reinterpret_cast<uint32_t*>(g_guestBase + va) = 3u;  // EXPECT: UNGATED
  uint8_t* q = h + 16;
  storeBE32(q, 7u);  // EXPECT: UNGATED
  // Controls: reads are not sites.
  const uint8_t* r = sr::kernel::guestVAToHost(va);
  uint8_t x = r[0];
  uint8_t buf[16];
  std::memcpy(buf, sr::kernel::guestVAToHost(va), 16);
  uint8_t* host = buf;
  host[1] = x;
  if (h[2] == 0) x = 1;
  bool flag = *sr::kernel::guestVAToHost(va) != 0;
  (void)flag;
}

// Controls: a pointer DECLARATION is not a store, and a lambda parameter that
// shares a name with a derived pointer is not that pointer.
namespace alt { thread_local uint8_t* g_guestBase = nullptr; }
void shadowCase(uint32_t va) {
  uint8_t* h = sr::kernel::guestVAToHost(va);
  auto put = [](uint8_t* h, uint8_t v) { h[0] = v; };
  (void)put;
  (void)h;
}

// A PPC override: `base` is the guest base here.
void sub_82000070(PPCContext& ctx, uint8_t* base) {
  uint8_t* p = base + ctx.r3.u32;
  p[0] = 0;  // EXPECT: UNGATED
  rawWrites(ctx.r3.u32, nullptr, 0);
  shadowCase(ctx.r3.u32);
}
