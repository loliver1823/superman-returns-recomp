// iter-467 native-a: p2.transport step 1 - the CALLBACK record's XDK form (kernel/native_flush.cpp callbackDwords, the
// kRkCallback renderer) against the guest GpuEmitCpuCallbackPackets_820FDA80 itself.
//
//   --callback-test
//     C1..C4  for the vblank gate's call (flags 0: the WAIT_UNTIL 3D-idle prefix, CPU 2), the Z-pass kick's (flags =
//             mask << 24 | 1: no prefix), a CPU-mask-only flags word (prefix, mask from bits 24..29) and a flags word
//             with bit 30 (no prefix): the guest body writes exactly the renderer's dwords at lastDw + 4 and returns
//             lastDw + 4 * n (23 or 25), and the bytes after the block are untouched
#include "ppc/ppc_config.h"
#include "ppc/ppc_context.h"
#include "kernel/guest_context.hpp"
#include "kernel/native_flush.hpp"
#include "kernel/stubs.hpp"

extern "C" void __imp__sub_820FDA80(PPCContext& __restrict ctx, uint8_t* base);  // the guest body (C linkage)

#include <cstdio>
#include <cstring>

namespace K = sr::kernel;

namespace {
int g_pass = 0, g_fail = 0;
void check(bool ok, const char* name) {
  if (ok) {
    g_pass++;
    std::printf("  ok  %s\n", name);
  } else {
    g_fail++;
    std::printf("FAIL: %s\n", name);
  }
}
uint32_t rdBE32(uint32_t va) {
  const uint8_t* p = K::guestVAToHost(va);
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}

constexpr uint32_t kDev = 0x70600000u;   // a scratch device: only dev+0x2A14 (the write-back block) is read
constexpr uint32_t kPc = 0x7FE14000u;    // its write-back block VA
constexpr uint32_t kBuf = 0x70640000u;   // the command buffer slot
constexpr uint32_t kStack = 0x70580000u;
}  // namespace

int runCallbackTest() {
  std::printf("[CALLBACK_TEST] the kRkCallback renderer vs the guest GpuEmitCpuCallbackPackets_820FDA80\n");
  K::registerStubs();
  K::writeGuestBE32(kDev + 0x2A14u, kPc);
  struct Case {
    const char* name;
    uint32_t flags, cb, arg;
  } cases[] = {
      {"C1 the vblank gate (flags 0: prefix, CPU 2)", 0u, 0x82111E38u, 0x00010008u},
      {"C2 the Z-pass kick (mask 4 << 24 | 1: no prefix)", 0x04000001u, 0x82110B00u, 0x1E8B0000u},
      {"C3 a mask-only flags word (CPU 0 | 1: prefix)", 0x03000000u, 0x82000000u, 0x12345678u},
      {"C4 flags bit 30 (no prefix, mask 0 -> CPU 2)", 0x40000000u, 0u, 0u},
  };
  char buf[240];
  for (const Case& c : cases) {
    std::memset(K::guestVAToHost(kBuf), 0xEE, 0x200);
    PPCContext ctx{};
    ctx.r1.u64 = kStack;
    ctx.r3.u64 = kDev;
    ctx.r4.u64 = kBuf;  // lastDw: the block lands at kBuf + 4
    ctx.r5.u64 = c.flags;
    ctx.r6.u64 = c.cb;
    ctx.r7.u64 = c.arg;
    __imp__sub_820FDA80(ctx, K::getGuestBase());
    const uint32_t m = (c.flags >> 24) & 0x3Fu;
    const uint32_t w[5] = {c.cb, c.arg, m ? m : 4u, c.flags,
                           ((((kPc >> 20) + 0x200u) & 0x1000u) + (kPc & 0x1FFFFFFFu)) | 2u};
    uint32_t v[25];
    const uint32_t n = sr::native::flush::callbackDwords(w, v);
    uint32_t firstBad = n;
    for (uint32_t i = 0; i < n && firstBad == n; i++)
      if (rdBE32(kBuf + 4u + 4u * i) != v[i]) firstBad = i;
    const bool r3ok = ctx.r3.u32 == kBuf + 4u * n;
    const bool after = rdBE32(kBuf + 4u + 4u * n) == 0xEEEEEEEEu;
    std::snprintf(buf, sizeof buf, "%s: %u dwords, %s, r3 %08X (want %08X), the next dword %s", c.name, n,
                  firstBad == n ? "byte-identical" : "DIFFER", ctx.r3.u32, kBuf + 4u * n, after ? "untouched" : "WRITTEN");
    if (firstBad != n)
      std::printf("      first differing dword %u: guest %08X renderer %08X\n", firstBad, rdBE32(kBuf + 4u + 4u * firstBad),
                  v[firstBad]);
    check(firstBad == n && r3ok && after, buf);
  }
  std::printf("[CALLBACK_TEST] %d passed, %d failed\n", g_pass, g_fail);
  return g_fail ? 1 : 0;
}
