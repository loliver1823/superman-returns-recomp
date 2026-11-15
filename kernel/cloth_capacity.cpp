#include "cloth_capacity.hpp"
#include "guest_context.hpp"
#include "kpcr.hpp"
#include "guest/ppc/ppc_recomp_shared.h"
#include "app/log.hpp"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>

extern "C" PPC_FUNC(__imp__sub_822C6E30);
#include "cloth_tangents_sized.inc"

namespace sr::kernel {
bool clothScratchLayout(uint32_t vertices, ClothScratchLayout* out) {
  if (!out || !vertices || vertices > kClothCapacityLimit) return false;
  const uint32_t uvBytes = std::max(1600u, (vertices * 8u + 15u) & ~15u);
  const uint32_t accum = std::max(4800u, vertices * 16u);
  *out = {816u + uvBytes, 816u + uvBytes + accum, accum,
          816u + uvBytes + 2u * accum + 96u};
  return true;
}

bool clothTangentsWithCapacity(PPCContext& ctx, uint8_t* base, std::string* error) {
  auto bad = [&](const char* why) { if (error) *error = why; return false; };
  auto range = [&](uint32_t va, uint32_t bytes) {
    return va && bytes && uint64_t(va) + bytes <= UINT32_MAX &&
        hostRangeReadable(guestVAToHost(va), bytes);
  };
  const uint32_t cloth = ctx.r3.u32;
  if (!range(cloth, 15572)) return bad("cloth descriptor is inaccessible");
  const uint32_t count = PPC_LOAD_U32(cloth + 15532);
  ClothScratchLayout layout{};
  if (!clothScratchLayout(count, &layout)) return bad("cloth vertex count exceeds the reviewed capacity");
  const uint32_t rss = PPC_LOAD_U32(cloth);
  if (!range(rss, 28) || PPC_LOAD_U32(rss + 16) != count || PPC_LOAD_U32(rss + 20) != 2)
    return bad("cloth RSS needs matching float2 UVs");
  if (!range(PPC_LOAD_U32(rss + 24), count * 8)) return bad("cloth UV array is inaccessible");
  for (uint32_t offset : {15500u, 15508u, 15512u})
    if (!range(PPC_LOAD_U32(cloth + offset), count * 16)) return bad("cloth vertex array is inaccessible");
  const uint32_t indices = PPC_LOAD_U32(cloth + 15564), indexCount = PPC_LOAD_U32(cloth + 15568);
  if (!indexCount || indexCount % 4 || indexCount > 4u * 65535u || !range(indices, indexCount * 2))
    return bad("cloth requires complete four-index faces");
  for (uint32_t i = 0; i < indexCount; ++i)
    if (PPC_LOAD_U16(indices + i * 2) >= count) return bad("cloth face index is out of range");
  const uint32_t pcr = ctx.r13.u32, sp = ctx.r1.u32;
  if (!range(pcr, kKpcrOffStackEnd + 4)) return bad("guest stack bounds are unavailable");
  const uint32_t hi = PPC_LOAD_U32(pcr + kKpcrOffStackBase);
  const uint32_t lo = PPC_LOAD_U32(pcr + kKpcrOffStackEnd);
  // Check ownership, not merely committed memory: adjacent guest heaps are writable too.
  if (!lo || lo >= hi || hi < 32 || sp > hi - 32 || sp < lo || sp - lo < layout.frame ||
      !range(sp - layout.frame, layout.frame + 32)) return bad("cloth scratch would exceed this thread's stack");
  clothTangentsSized(ctx, base, layout);
  return true;
}
}

PPC_FUNC(sub_822C6E30) {
  // iter-465 fortress: RETIRED EXPERIMENT, default OFF for everyone. The costume packs' capes now
  // fit the engine's own limit (<= 200 vertices; the tested packs use a 187-vertex cape), so
  // the retail body always runs. SR_CLOTH_CAPACITY=1 keeps the resized body for A/B (read per
  // call: difftest flips it). At <= 200 vertices the original body runs either way.
  const char* gate = std::getenv("SR_CLOTH_CAPACITY");
  const bool on = gate && gate[0] == '1';
  if (!on) { __imp__sub_822C6E30(ctx, base); return; }
  const uint32_t cloth = ctx.r3.u32;
  if (sr::kernel::hostRangeReadable(sr::kernel::guestVAToHost(cloth), 15536) &&
      PPC_LOAD_U32(cloth + 15532) <= 200) {
    __imp__sub_822C6E30(ctx, base);
    return;
  }
  std::string error;
  if (!sr::kernel::clothTangentsWithCapacity(ctx, base, &error)) {
    std::fprintf(stderr, "[FATAL] [CLOTH_CAPACITY] %s\n", error.c_str());
    std::fflush(stderr);
    std::abort();
  }
  static std::atomic<unsigned> logged{0};
  if (logged.fetch_add(1, std::memory_order_relaxed) < 20) {
    const uint32_t count = PPC_LOAD_U32(cloth + 15532);
    sr::kernel::ClothScratchLayout layout{};
    sr::kernel::clothScratchLayout(count, &layout);
    SR_LOG_MEM("[CLOTH_CAPACITY] vertices=%u scratch_frame=%u original_frame=12112 port_feature=1", count, layout.frame);
  }
}
