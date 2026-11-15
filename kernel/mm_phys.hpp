// iter-463 PLATFORM FIX: Xenia-faithful guest physical addresses (MmGetPhysicalAddress).
//
// Ported semantics from Xenia Canary (BSD-3-Clause, see docs/THIRD_PARTY.md), oracle build
// canary_experimental@0bd090dbe:
//   src/xenia/memory.cc:441-462    Memory::LookupHeap (which heap owns a VA)
//   src/xenia/memory.cc:526-538    Memory::GetPhysicalAddress
//   src/xenia/memory.cc:2434-2442  PhysicalHeap::GetPhysicalAddress (+0x1000 for the 0xE heap)
//   src/xenia/memory.cc:211-232    heap bases
//   src/xenia/kernel/xboxkrnl/xboxkrnl_memory.cc:643-655  MmGetPhysicalAddress (UINT32_MAX -> 0)
//
// xeGetPhysicalAddress (always Xenia semantics, NO gate) is what the XMA exports use internally
// (critique B2: they must never see the gated export's legacy 0).
//
// SR_MM_PHYSADDR (default ON since the iter-463 rolls; =0 keeps the generic stub that
// returns 0): the MmGetPhysicalAddress EXPORT. Live callers (IDA xrefs to the import 0x828EE1FC):
//   D3DDeviceInitRingBuffer_sub_820FDE90 @0x820FE05C (-> VdInitializeRingBuffer r3; our stub
//     falls back to g_gpuRingAllocVA for any r3 below 0x82000000, which covers 0 AND every real
//     physical address, so the ring base is unchanged either way);
//   Xma_AllocContextBlock_sub_82119748 @0x821198BC/0x82119900, Xma_CreateContexts_sub_82119980
//     @0x821199E8, Xma_SubmitInputBuffer_sub_82119AC8 @0x82119B08/0x82119B38 - the XDK XAudio
//     XMA HAL, DORMANT in this game (docs/ENGINE_MAP.md section 7a).
//
// Kernel-only header (never add these to guest_context.hpp).
#pragma once

#include <cstdint>

namespace sr::kernel {

class ExportRegistry;

// Memory::GetPhysicalAddress: UINT32_MAX when no heap owns `va` (0x7FC80000..0x7FFFFFFF and
// 0xFFD00000..0xFFFFFFFF); `va` itself for a non-physical heap below 0x1FFFFFFF; otherwise
// va - heap_base, +0x1000 for the 0xE0000000 heap.
uint32_t xeGetPhysicalAddress(uint32_t va);

// The export body: xeGetPhysicalAddress with UINT32_MAX mapped to 0.
uint32_t mmGetPhysicalAddressExport(uint32_t va);

bool mmPhysAddrEnabled();                 // SR_MM_PHYSADDR (default ON)
void mmPhysAddrSetEnabledForTest(int mode);   // -1 env, 0 off, 1 on

// Overrides the "MmGetPhysicalAddress" registration ONLY when mmPhysAddrEnabled(); with the gate
// off the generic stub registered before it stays in place, byte for byte.
void registerMmPhysExport(ExportRegistry& reg);

}  // namespace sr::kernel
