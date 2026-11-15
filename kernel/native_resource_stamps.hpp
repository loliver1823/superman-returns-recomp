#pragma once
// mem.resources (native routing): the D3D driver's CPU stores into resource headers that no
// coherency record announces. The bodies of SetStreamSource 820F2A50 (store 820F2AE0), SetIndices
// 820F2C08 (820F2C34), SetTexture 82100310 (82100428), SetPixelShader 820F4E88 (820F4EB4) and
// SetVertexShader 820F5218 read the object bound in a device slot and, when the device's current
// fence dev+0x2A1C is nonzero, store that fence at object+8 - the OUTGOING object's last-use fence
// (verified in IDA 2026-10-17: every `lwz rX,0x2A1C(dev)` feeding a `stw rX,8(obj)` in the recompiled
// image, archive/ida/fence_stamp_candidates.txt of the mem-native worktree). The headers share 4 KiB
// pages with vertex data in the title's resource arenas, so the GPU's view of those pages must see
// the stamp as RAM holds it (the console GPU reads unified memory): an EXACT dword note
// (gfx/resource_memory.hpp notePatch), never a page note - the page can hold GPU-written bytes.
//
// Use: read the target BEFORE the body (it rebinds the slot) - resourceFenceStampTarget notes the dword then
// already - and note it again AFTER the body returned (the store completed). The two notes bracket the store: a
// request between them takes the first and copies the dword as RAM holds it at that moment, the second covers the
// store itself (a note only after it left a window - store, a CP decision, note - where the page read changed with
// nothing pending: lane D's mrq1_ww census, ingame missed=15). Observe-only: two guest reads, the body runs
// unchanged. Active when the native
// resource route runs (resourceMode(): always in the release build, SR_MEM_RESOURCES=1 in the dev
// build) and in the census's shadow (SR_MEM_RESOURCES=census).
#include <cstdint>

namespace sr::kernel {
constexpr uint32_t kDevStreamSlots = 0x2F9C;    // + 4 * stream (SetStreamSource: dev + 4 * (stream + 3047))
constexpr uint32_t kDevIndexSlot = 0x2F84;      // SetIndices
constexpr uint32_t kDevTextureSlots = 0x2FF0;   // + 4 * sampler (SetTexture: dev + 4 * (slot + 0xBFC))
constexpr uint32_t kDevPixelShaderSlot = 0x3080, kDevVertexShaderSlot = 0x3084;
constexpr uint32_t kDevCurrentFence = 0x2A1C;
constexpr uint32_t kStreamSlots = 16, kTextureSlots = 32;

// The guest address the body will stamp (object + 8), or 0 when it stamps nothing.
uint32_t resourceFenceStampTarget(uint32_t device, uint32_t slotOffset) noexcept;
void resourceFenceStampDone(uint32_t target) noexcept;
// The census's name for RenderModel_SwapMeshChunkBindings' CPU-only draw records (the .cpp's comment; Oracle only).
void resourceCpuOnlyRecords(uint32_t model, uint32_t row) noexcept;
inline uint32_t resourceStreamStampTarget(uint32_t device, uint32_t stream) noexcept {
  return stream < kStreamSlots ? resourceFenceStampTarget(device, kDevStreamSlots + 4u * stream) : 0u;
}
inline uint32_t resourceTextureStampTarget(uint32_t device, uint32_t slot) noexcept {
  return slot < kTextureSlots ? resourceFenceStampTarget(device, kDevTextureSlots + 4u * slot) : 0u;
}
}  // namespace sr::kernel
