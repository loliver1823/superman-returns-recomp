// iter-467 native-a: the shader time 0x29 held with the sim (kernel/timebase_obs.cpp, docs/HIGH_FRAMERATE.md).
//
//   --shtime-hold-test      (SR_DIAG_SIM_FREEZE unset: every player's run)
//     P0  the freeze diagnostic is off in this process
//     P1  ShaderParam_WriteTimeSeconds (sub_8260DDC8, the override) and its guest body, called alternately: every
//         float4 the override writes is one the guest body's own clock reading produces - its .x is bracketed,
//         bit for bit, by the guest body's writes just before and after it (the clock is monotonic), and .y / .z
//         / .w and the dirty words are identical - for VS and PS registers
//     P2  no QueryPerformanceCounter read was substituted (sr::shtime::substitutions() == 0): the overrides ran the
//         guest bodies and nothing else
//     P3  QueryPerformanceCounter (sub_824680D0): the live guest mftb, in [before, after], r3 = 1
//   --shtime-hold-test-on   (the test sets SR_DIAG_SIM_FREEZE before anything reads it: the diagnostic's path)
//     Q1  the hold's substitution feeds the guest body's own arithmetic: with the QPC pinned, .x is exactly
//         float((QPC - qword 8299AEA0) / 50e6) and every write under one pin is the same float (a hold draws the
//         same .x on every pass)
//     Q2  the override outside a hold (no hold yet: the held time is 0) writes the live guest clock, bracketed as
//         in P1, and its QPC read went through the substitution (the counter moved)
#include "ppc/ppc_config.h"
#include "ppc/ppc_context.h"
#include "kernel/guest_context.hpp"
#include "kernel/highfps.hpp"
#include "kernel/shtime_hold.hpp"
#include "kernel/sr_timebase.hpp"
#include "kernel/stubs.hpp"

PPC_EXTERN_FUNC(sub_824680D0);   // QueryPerformanceCounter: the override (kernel/timebase_obs.cpp)
PPC_EXTERN_FUNC(sub_8260DDC8);   // ShaderParam_WriteTimeSeconds: the override
extern "C" void __imp__sub_8260DDC8(PPCContext& __restrict ctx, uint8_t* base);  // its guest body (C linkage)

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace K = sr::kernel;

namespace {
int g_pass = 0, g_fail = 0;
void check(bool ok, const char* name) {
  if (ok) { g_pass++; std::printf("  ok  %s\n", name); }
  else { g_fail++; std::printf("FAIL: %s\n", name); }
}
uint64_t rdBE64(uint32_t va) {
  const uint8_t* p = K::guestVAToHost(va);
  uint64_t v = 0;
  for (int i = 0; i < 8; i++) v = (v << 8) | p[i];
  return v;
}
void wrBE64(uint32_t va, uint64_t v) {
  uint8_t* p = K::guestVAToHost(va);
  for (int i = 0; i < 8; i++) p[i] = uint8_t(v >> (56 - 8 * i));
}
uint32_t rdBE32(uint32_t va) {
  const uint8_t* p = K::guestVAToHost(va);
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}
float asFloat(uint32_t bits) {
  float f;
  std::memcpy(&f, &bits, 4);
  return f;
}

constexpr uint32_t kDevPtr = 0x8299ADF4u;  // dword_8299ADF4: the render device
constexpr uint32_t kBaseQ = 0x8299AEA0u;   // qword_8299AEA0: the device-init QPC stamp
constexpr uint32_t kDev = 0x70600000u;     // a scratch device (constant shadow up to +0x1700 + 16 * 256)
constexpr uint32_t kStack = 0x70580000u;
constexpr uint32_t kQpcOut = 0x70700000u;

struct Shot { uint32_t w[4]; uint64_t dirty0, dirty8; };
Shot callWrite(bool override_, uint32_t reg, bool vs) {
  std::memset(K::guestVAToHost(kDev), 0, 0x1800 + 16 * 256);
  PPCContext ctx{};
  ctx.r1.u64 = kStack;
  ctx.r3.u64 = reg;
  ctx.r5.u64 = vs ? 1 : 0;
  if (override_) sub_8260DDC8(ctx, K::getGuestBase());
  else __imp__sub_8260DDC8(ctx, K::getGuestBase());
  Shot s{};
  const uint32_t slot = kDev + 16u * (reg + (vs ? 0x70u : 0x170u));
  for (int i = 0; i < 4; i++) s.w[i] = rdBE32(slot + 4u * i);
  s.dirty0 = rdBE64(kDev);
  s.dirty8 = rdBE64(kDev + 8);
  return s;
}

struct Scene { uint32_t savedDev; uint64_t savedBase, base; };
Scene setUp() {
  Scene sc{rdBE32(kDevPtr), rdBE64(kBaseQ), 0};
  K::writeGuestBE32(kDevPtr, kDev);
  sc.base = sr_guest_mftb() - 50000000ull * 37;  // "the device came up 37 s ago"
  wrBE64(kBaseQ, sc.base);
  return sc;
}
void tearDown(const Scene& sc) {
  K::writeGuestBE32(kDevPtr, sc.savedDev);
  wrBE64(kBaseQ, sc.savedBase);
}

// P1 / Q2: the override's write is bracketed by guest-body writes (monotonic clock), the rest identical.
bool bracketed(int& n, int& total) {
  n = total = 0;
  for (uint32_t reg : {0u, 3u, 17u, 255u})
    for (bool vs : {true, false}) {
      const Shot g0 = callWrite(false, reg, vs);
      const Shot o = callWrite(true, reg, vs);
      const Shot g1 = callWrite(false, reg, vs);
      total++;
      const bool inside = asFloat(g0.w[0]) <= asFloat(o.w[0]) && asFloat(o.w[0]) <= asFloat(g1.w[0]);
      const bool rest = !std::memcmp(o.w + 1, g0.w + 1, 12) && o.dirty0 == g0.dirty0 && o.dirty8 == g0.dirty8;
      if (inside && rest) n++;
    }
  return n == total;
}
}  // namespace

int runShtimeHoldTest(bool on) {
  if (on) _putenv_s("SR_DIAG_SIM_FREEZE", "8:2000:100000");  // before anything reads it (the gates latch)
  std::printf("[SHTIME_HOLD_TEST] the shader time 0x29, SR_DIAG_SIM_FREEZE %s\n",
              on ? "SET (the diagnostic)" : "unset (the player's path)");
  K::registerStubs();
  sr::timebase::reinitForTest(true);
  char buf[240];
  const Scene sc = setUp();
  int n = 0, total = 0;
  if (!on) {
    check(!K::highfps::simFreezeOn(), "P0 SR_DIAG_SIM_FREEZE is not configured in this process");
    const bool ok = bracketed(n, total);
    std::snprintf(buf, sizeof buf, "P1 override write = a guest-body write (.x bracketed bit for bit, .yzw + dirty identical): %d of %d",
                  n, total);
    check(ok, buf);
    std::snprintf(buf, sizeof buf, "P2 QPC reads substituted: %llu (want 0)", (unsigned long long)sr::shtime::substitutions());
    check(sr::shtime::substitutions() == 0, buf);
    PPCContext ctx{};
    ctx.r3.u64 = kQpcOut;
    const uint64_t a0 = sr_guest_mftb();
    sub_824680D0(ctx, K::getGuestBase());
    const uint64_t q = rdBE64(kQpcOut), a1 = sr_guest_mftb();
    std::snprintf(buf, sizeof buf, "P3 QPC = live mftb %llu in [%llu, %llu], r3 = %llu", (unsigned long long)q,
                  (unsigned long long)a0, (unsigned long long)a1, (unsigned long long)ctx.r3.u64);
    check(a0 <= q && q <= a1 && ctx.r3.u64 == 1 && sr::shtime::substitutions() == 0, buf);
  } else {
    check(K::highfps::simFreezeOn(), "Q0 SR_DIAG_SIM_FREEZE is configured in this process");
    // Q1: a pinned QPC through the guest body = the guest's arithmetic, the same float on every write.
    const uint64_t pin = sc.base + 50000000ull * 37 + 777;
    const uint64_t s0 = sr::shtime::substitutions();
    sr::shtime::armFixedForTest(pin);
    const Shot h0 = callWrite(false, 5, false);
    const Shot h1 = callWrite(false, 5, false);
    const Shot h2 = callWrite(false, 9, true);
    sr::shtime::disarmForTest();
    const float want = float(double(int64_t(pin - sc.base)) / double(50000000ull));
    uint32_t wantBits;
    std::memcpy(&wantBits, &want, 4);
    std::snprintf(buf, sizeof buf, "Q1 pinned QPC: .x = %08X %08X %08X (want %08X = %.6f s), %llu substituted reads",
                  h0.w[0], h1.w[0], h2.w[0], wantBits, want, (unsigned long long)(sr::shtime::substitutions() - s0));
    check(h0.w[0] == wantBits && h1.w[0] == wantBits && h2.w[0] == wantBits && sr::shtime::substitutions() - s0 == 3,
          buf);
    // Q2: the override outside a hold, the held time 0: the live clock, through the substitution.
    const uint64_t s1 = sr::shtime::substitutions();
    const bool ok = bracketed(n, total);
    std::snprintf(buf, sizeof buf,
                  "Q2 override, no hold yet: a live-clock write (bracketed, .yzw + dirty identical) %d of %d, %llu substituted reads",
                  n, total, (unsigned long long)(sr::shtime::substitutions() - s1));
    check(ok && sr::shtime::substitutions() - s1 == uint64_t(total), buf);
  }
  tearDown(sc);
  std::printf("[SHTIME_HOLD_TEST] %d passed, %d failed\n", g_pass, g_fail);
  return g_fail ? 1 : 0;
}
