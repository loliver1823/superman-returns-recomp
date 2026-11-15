// Execute the actual original and resized game bodies; no substituted leaves.
// Synthetic plane quads test scratch storage, not live cloth simulation/rendering.
#include "kernel/cloth_capacity.hpp"
#include "kernel/guest_context.hpp"
#include "kernel/kpcr.hpp"
#include "ppc/ppc_recomp_shared.h"

#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <vector>

extern "C" PPC_FUNC(__imp__sub_822C6E30);
namespace K = sr::kernel;
namespace {
constexpr uint32_t arena = 0x90900000, arenaSize = 0x100000;
constexpr uint32_t cloth = arena, rss = arena + 0x4000, pcr = arena + 0x5000;
constexpr uint32_t positions = arena + 0x10000, normals = arena + 0x20000;
constexpr uint32_t tangents = arena + 0x30000, uv = arena + 0x41000, indices = arena + 0x51000;
constexpr uint32_t stackLo = arena + 0x80000, stackHi = arena + arenaSize, sp = stackHi - 0x100;
int pass = 0, fail = 0;
std::array<PPCRegister*, 10> nonvolatile(PPCContext& ctx) {
  return {&ctx.r22, &ctx.r23, &ctx.r24, &ctx.r25, &ctx.r26,
          &ctx.r27, &ctx.r28, &ctx.r29, &ctx.r30, &ctx.r31};
}
void check(bool ok, const char* what) {
  std::printf("%s %s\n", ok ? "PASS" : "FAIL", what);
  (ok ? pass : fail)++;
}
void w32(uint32_t va, uint32_t value) { K::writeGuestBE32(va, value); }
void wf(uint32_t va, float value) {
  uint32_t bits; std::memcpy(&bits, &value, 4); w32(va, bits);
}
PPCContext fixture(uint32_t count) {
  uint8_t* base = K::getGuestBase();
  std::memset(K::guestVAToHost(arena), 0, arenaSize);
  w32(cloth, rss); w32(cloth + 15500, positions);
  w32(cloth + 15508, normals); w32(cloth + 15512, tangents);
  w32(cloth + 15532, count); w32(cloth + 15564, indices); w32(cloth + 15568, count * 2);
  w32(rss + 16, count); w32(rss + 20, 2); w32(rss + 24, uv);
  w32(pcr + K::kKpcrOffStackBase, stackHi); w32(pcr + K::kKpcrOffStackEnd, stackLo);
  wf(0x820013C0, 1.0f);
  const std::array<std::array<float, 2>, 4> corners{{{0,0},{1,0},{1,1},{0,1}}};
  for (uint32_t i = 0; i < count; ++i) {
    const auto& point = corners[i % 4];
    wf(positions + i * 16, point[0]); wf(positions + i * 16 + 4, point[1]);
    wf(positions + i * 16 + 12, 1);
    wf(normals + i * 16 + 8, 1);
    wf(uv + i * 8, point[0]); wf(uv + i * 8 + 4, point[1]);
    // The unchanged indexed routine accumulates at corners 1/2/3. Two
    // cyclic orientations ensure every vertex has a nonzero contribution,
    // as a connected cloth surface does; isolated single quads do not.
    const uint32_t face = i / 4, corner = i % 4;
    PPC_STORE_U16(indices + (face * 8 + corner) * 2, i);
    PPC_STORE_U16(indices + (face * 8 + 4 + corner) * 2, face * 4 + (corner + 2) % 4);
  }
  std::memset(K::guestVAToHost(tangents), 0xCD, count * 16 + 16);
  PPCContext ctx{};
  ctx.r1.u32 = sp; ctx.r3.u32 = cloth; ctx.r13.u32 = pcr; ctx.lr = 0x82123450;
  auto registers = nonvolatile(ctx);
  for (unsigned i = 0; i < registers.size(); ++i) registers[i]->u64 = 0x100000000ull + i;
  ctx.f31.f64 = 3.125;
  return ctx;
}
bool saved(const PPCContext& ctx) {
  if (ctx.r1.u32 != sp || ctx.lr != 0x82123450 || ctx.f31.f64 != 3.125) return false;
  const std::array<const PPCRegister*, 10> registers{&ctx.r22, &ctx.r23, &ctx.r24, &ctx.r25, &ctx.r26,
          &ctx.r27, &ctx.r28, &ctx.r29, &ctx.r30, &ctx.r31};
  for (unsigned i = 0; i < registers.size(); ++i)
    if (registers[i]->u64 != 0x100000000ull + i) return false;
  return true;
}
bool finiteTangents(uint32_t count) {
  uint8_t* base = K::getGuestBase();
  for (uint32_t i = 0; i < count * 4; ++i) {
    uint32_t bits = PPC_LOAD_U32(tangents + i * 4); float value;
    std::memcpy(&value, &bits, 4);
    if (!std::isfinite(value)) return false;
  }
  return true;
}
bool sentinel(uint32_t count) {
  auto* p = static_cast<uint8_t*>(K::guestVAToHost(tangents + count * 16));
  for (unsigned i = 0; i < 16; ++i) if (p[i] != 0xCD) return false;
  return true;
}
}
int runClothCapacityTest() {
  uint8_t* base = K::getGuestBase();
  if (!base || !K::hostRangeReadable(K::guestVAToHost(arena), arenaSize)) {
    std::puts("FAIL cloth test needs the runner's full guest mapping"); return 1;
  }
  std::vector<uint8_t> oldArena(static_cast<uint8_t*>(K::guestVAToHost(arena)),
      static_cast<uint8_t*>(K::guestVAToHost(arena)) + arenaSize);
  const uint32_t oldOne = PPC_LOAD_U32(0x820013C0);
  for (uint32_t count : {4u, 160u, 200u}) {
    PPCContext original = fixture(count);
    __imp__sub_822C6E30(original, base);
    check(saved(original), "original preserves ABI");
    std::vector<uint8_t> expected(static_cast<uint8_t*>(K::guestVAToHost(tangents)),
        static_cast<uint8_t*>(K::guestVAToHost(tangents)) + count * 16);
    PPCContext resized = fixture(count); std::string error;
    check(K::clothTangentsWithCapacity(resized, base, &error), "resized body executes");
    check(!std::memcmp(expected.data(), K::guestVAToHost(tangents), expected.size()), "retail-capacity output bit exact");
    check(saved(resized) && sentinel(count), "resized preserves ABI and output boundary");
    check(finiteTangents(count), "retail-capacity tangents finite");
  }
  // A small overrun cannot reach saved registers here. It does overlap UVs with
  // the original accumulation memset: the old routine loses the last UVs.
  PPCContext broken = fixture(204);
  __imp__sub_822C6E30(broken, base);
  check(!finiteTangents(204), "negative control: original UV scratch fails at 204 vertices");
  for (uint32_t count : {204u, 2004u, 4096u}) {
    PPCContext ctx = fixture(count); std::string error;
    check(K::clothTangentsWithCapacity(ctx, base, &error), "large cloth executes");
    check(finiteTangents(count), "large cloth tangents finite");
    check(saved(ctx) && sentinel(count), "large cloth ABI and output boundary intact");
  }
  const char* previousGate = std::getenv("SR_CLOTH_CAPACITY");
  const std::string oldGate = previousGate ? previousGate : "";
  for (const char* value : {"0", "1"}) {
    _putenv_s("SR_CLOTH_CAPACITY", value);
    PPCContext ctx = fixture(204);
    sub_822C6E30(ctx, base);
    check(finiteTangents(204) == (value[0] == '1'), "public guest entry obeys the capacity gate");
  }
  _putenv_s("SR_CLOTH_CAPACITY", oldGate.c_str());
  for (unsigned variant = 0; variant < 5; ++variant) {
    PPCContext ctx = fixture(204); std::string error;
    switch (variant) {
      case 0: w32(cloth + 15532, 4097); break;
      case 1: w32(rss + 20, 4); break;
      case 2: PPC_STORE_U16(indices, 204); break;
      case 3: w32(cloth + 15568, 203); break;
      case 4: w32(pcr + K::kKpcrOffStackEnd, sp - 12112); break;
    }
    check(!K::clothTangentsWithCapacity(ctx, base, &error) && !error.empty(), "malformed or unowned scratch refused");
    check(saved(ctx) && *static_cast<uint8_t*>(K::guestVAToHost(tangents)) == 0xCD,
          "refusal does not execute or change output");
  }
  std::memcpy(K::guestVAToHost(arena), oldArena.data(), arenaSize);
  w32(0x820013C0, oldOne);
  std::printf("cloth capacity: %d pass, %d fail\n", pass, fail);
  return fail ? 1 : 0;
}
