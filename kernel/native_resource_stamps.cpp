// mem.resources (native routing): the D3D driver's header fence stamps as exact CPU-write patches.
// The contract and the IDA evidence: kernel/native_resource_stamps.hpp.
#include "native_resource_stamps.hpp"

#include "guest/ppc/ppc_recomp_shared.h"
#include "guest_context.hpp"
#include "sr_oracle.hpp"
#include "../gfx/resource_census.hpp"
#include "../gfx/resource_memory.hpp"
#include "../app/log.hpp"
#include <atomic>

namespace sr::kernel {

uint32_t resourceFenceStampTarget(uint32_t device, uint32_t slotOffset) noexcept {
  if (!gfx::memres::resourceMode() && !gfx::memres::census()) return 0;
  // A device the title could not have created (the SetStreamSource junk storm the iter-449w
  // tripwire counts in the dev build) is not read: the body reads it on its own.
  if (device < 0x40000000u || device >= 0xC0000000u) return 0;
  const uint32_t object = readGuestU32BE(device + slotOffset);
  const uint32_t fence = readGuestU32BE(device + kDevCurrentFence);
  const uint32_t target = object && fence ? object + 8u : 0u;
  resourceFenceStampDone(target);  // the first note of the bracket (the header comment)
  return target;
}

namespace {
// step 6 G8 (SR_PATCH_LEAN): the bracket runs twice a shader bind, so the route and the arena are read once - both are
// fixed at their first read (resourceMode / census read their gates once; resourceMemory() is the one arena).
struct StampRoute {
  bool lean;
  gfx::memres::ResourceMemory* memory;  // the native route's arena, or null
  bool census;
};
const StampRoute& stampRoute() noexcept {
  static const StampRoute route = [] {
    const bool native = gfx::memres::resourceMode();
    return StampRoute{gfx::memres::patchLeanOn(), native ? &gfx::memres::resourceMemory() : nullptr,
                      gfx::memres::census()};
  }();
  return route;
}
}  // namespace

void resourceFenceStampDone(uint32_t target) noexcept {
  if (!target) return;
  const StampRoute& route = stampRoute();
  if (route.lean) {
    if (route.memory) route.memory->notePatch(target, 4u);
    if (route.census) gfx::memres::notePatch(target, 4u);
    return;
  }
  if (gfx::memres::resourceMode()) gfx::memres::resourceMemory().notePatch(target, 4u);
  if (gfx::memres::census()) gfx::memres::notePatch(target, 4u);
}

#if SR_ORACLE
namespace {
uint32_t u16be(uint32_t va) noexcept {
  const uint32_t w = readGuestU32BE(va & ~3u);
  return (va & 2u) ? (w & 0xFFFFu) : (w >> 16);
}
uint32_t u8be(uint32_t va) noexcept { return (readGuestU32BE(va & ~3u) >> (24u - 8u * (va & 3u))) & 0xFFu; }
bool plausible(uint32_t va) noexcept { return va >= 0x00010000u && va < 0xF0000000u; }  // (models live at 0x02xxxxxx)
}  // namespace

// mem.resources census (the orchestrator's 2026-10-17 decision): RenderModel_SwapMeshChunkBindings (sub_822EBC98,
// IDA) - for model r3, variant row r4, no-op unless the override table +0x54 is non-null and flag byte +0x6C is set;
// over the meshes of the row (u16 remap table +0x40 if present, else all +8 meshes of array +0xC) it walks each mesh's
// chunks (from (mesh + 0x83) & ~15, flag 0x200000 -> the draw record at chunk + chunk[7], to the mesh's +8 end) and
// swaps record dwords +8 / +0xC / +0x10 with the 96-byte-stride table entry's +0 / +8. The record is the CPU's draw
// metadata (a CPU pointer among it): the GPU never reads it (no Warworld-era request marks it; 896 sampled draws'
// exact read sets exclude it). After the body, the same walk names those dwords to the census (noteCpuOnly) - it
// writes nothing and notes nothing (the registry never copies them). Bounded: 4096 meshes, 4096 chunks a mesh.
void resourceCpuOnlyRecords(uint32_t model, uint32_t row) noexcept {
  if (!gfx::memres::census()) return;
  // the first calls, for the roll's reader: the walk's inputs (a walk that names nothing is visible)
  static std::atomic<uint32_t> calls{0};
  const uint32_t n = calls.fetch_add(1, std::memory_order_relaxed) + 1u;
  const uint32_t table = readGuestU32BE(model + 0x54u), flag = u8be(model + 0x6Cu);
  const uint32_t remap = readGuestU32BE(model + 0x40u);
  const uint32_t meshes = remap ? u16be(remap + 10u) : readGuestU32BE(model + 8u);
  const uint32_t meshArr = readGuestU32BE(model + 12u), rowStride = readGuestU32BE(model + 96u);
  if (n <= 4u || (n & (n - 1u)) == 0u)
    SR_LOG_BOOT(sr::LogSeverity::Info,
                "[MEM_RES] CPU_ONLY swapmesh #%u model=%08X row=%u table=%08X flag=%u remap=%08X meshes=%u meshes_at=%08X "
                "row_stride=%u", n, model, row, table, flag, remap, meshes, meshArr, rowStride);
  if (!plausible(model) || !table || !flag) return;
  if (!plausible(meshArr) || (remap && !plausible(remap))) return;
  for (uint32_t i = 0; i < meshes && i < 4096u; i++) {
    uint32_t mi = i;
    if (remap) {
      const uint32_t idxTable = readGuestU32BE(remap + 12u);
      mi = u16be(remap + u16be(idxTable + 2u * (u16be(remap + 10u) * row + i)));
    }
    const uint32_t mesh = readGuestU32BE(meshArr + 4u * mi);
    if (!plausible(mesh)) continue;
    const uint32_t end = readGuestU32BE(mesh + 8u);
    uint32_t entry = 96u * rowStride * mi;
    uint32_t chunk = (mesh + 131u) & ~15u;
    for (uint32_t k = 0; k < 4096u && plausible(chunk); k++) {
      const uint32_t flags = readGuestU32BE(chunk), rel = readGuestU32BE(chunk + 28u);
      if (flags & 0x200000u) {
        gfx::memres::noteCpuOnly(chunk + rel + 8u, 12u);  // the record's +8 / +0xC / +0x10
        const uint32_t e = table + entry;
        if (plausible(e)) {
          gfx::memres::noteCpuOnly(e, 4u);       // the table entry's +0
          gfx::memres::noteCpuOnly(e + 8u, 4u);  // ... and +8
        }
      }
      entry += 96u;
      uint32_t next = chunk + rel;
      if (flags & 0x200000u) next += u16be(next);
      if (next >= end) break;
      chunk = (next + 15u) & ~15u;
    }
  }
}
#endif

}  // namespace sr::kernel

#if SR_ORACLE
// The census's name for the CPU-only draw records (resourceCpuOnlyRecords above): observe-only, after the body.
extern "C" void __imp__sub_822EBC98(PPCContext& __restrict ctx, uint8_t* base);
PPC_FUNC(sub_822EBC98) {  // RenderModel_SwapMeshChunkBindings(model, row)
  const uint32_t model = ctx.r3.u32, row = ctx.r4.u32;
  __imp__sub_822EBC98(ctx, base);
  sr::kernel::resourceCpuOnlyRecords(model, row);
}
#endif

#if !SR_ORACLE
// The release build: these bodies have no other override (the development build's observers in
// kernel/hle_api_observe.cpp and kernel/cs_timeout_underflow_fix.cpp make the same two calls around
// their delegation and are compiled out here). Each delegates to the recompiled body unchanged.
extern "C" void __imp__sub_820F2A50(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_820F2C08(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_82100310(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_820F4E88(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_820F5218(PPCContext& __restrict ctx, uint8_t* base);

PPC_FUNC(sub_820F2A50) {  // D3DDevice_SetStreamSource(dev, stream, vb, offset, stride, dirty)
  const uint32_t stamp = sr::kernel::resourceStreamStampTarget(ctx.r3.u32, ctx.r4.u32);
  __imp__sub_820F2A50(ctx, base);
  sr::kernel::resourceFenceStampDone(stamp);
}

PPC_FUNC(sub_820F2C08) {  // D3DDevice_SetIndices(dev, ib)
  const uint32_t stamp = sr::kernel::resourceFenceStampTarget(ctx.r3.u32, sr::kernel::kDevIndexSlot);
  __imp__sub_820F2C08(ctx, base);
  sr::kernel::resourceFenceStampDone(stamp);
}

PPC_FUNC(sub_82100310) {  // D3DDevice_SetTexture(dev, sampler, texture, dirty)
  const uint32_t stamp = sr::kernel::resourceTextureStampTarget(ctx.r3.u32, ctx.r4.u32);
  __imp__sub_82100310(ctx, base);
  sr::kernel::resourceFenceStampDone(stamp);
}

PPC_FUNC(sub_820F4E88) {  // D3DDevice_SetPixelShader(dev, ps)
  const uint32_t stamp = sr::kernel::resourceFenceStampTarget(ctx.r3.u32, sr::kernel::kDevPixelShaderSlot);
  __imp__sub_820F4E88(ctx, base);
  sr::kernel::resourceFenceStampDone(stamp);
}

PPC_FUNC(sub_820F5218) {  // D3DDevice_SetVertexShader(dev, vs)
  const uint32_t stamp = sr::kernel::resourceFenceStampTarget(ctx.r3.u32, sr::kernel::kDevVertexShaderSlot);
  __imp__sub_820F5218(ctx, base);
  sr::kernel::resourceFenceStampDone(stamp);
}
#endif
