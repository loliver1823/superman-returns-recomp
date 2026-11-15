#pragma once

// NATIVE (docs/NATIVE_ROADMAP.md §4.4, status "next step 1"): host port of the XDK's
// vertex-fetch patcher ShaderAssembleVertexFetches_sub_82107A30. The XDK patches a
// vertex shader's vfetch instructions for the bound vertex declaration and stream
// strides at draw time; the GPU runs the patched bytes. A native renderer needs the
// same bytes without running the XDK (and without touching the live shader object,
// which the CP may still be reading), so the port writes into a HOST copy of the ucode.
//
// Inputs (all read from guest memory, never written):
//   vs        the vertex shader object (variant container at vs + *(vs+8*(variant+112)) + 872,
//             its vfetch table, the cached fetch words at vs + 416*variant + 68)
//   decl      the vertex declaration (element count +24, elements from +52, 12 bytes each)
//   strides   the device's 16 stream-stride bytes (dev+12256, in dwords), indexed by the
//             element's stream; for DrawVerticesUP/BeginVertices the caller puts the call's
//             stride/4 in stream 0, as the XDK does for the duration of the call
//   garbage   the uninitialised stack halfword the guest reads at 0x82107A90 (lhz r8,80(r1));
//             provably irrelevant (see the .cpp), a parameter so a test can vary it
// Output: 12-byte fetch instructions written big-endian into `out` at
//   outBias + 12 * (entry & 0xFFF), exactly the guest's final memcpy step (the guest writes
//   at a2 + 12*(entry & 0xFFF), a2 = *(vs+32) + *(container+0); outBias = a2 - outBaseVA).
// Returns false (nothing written) for inputs the port does not handle (more entries than
// the guest's 124-dword stack array, a write outside `out`, unmapped memory).

#include <cstddef>
#include <cstdint>

namespace sr::native::vspatch {

// The guest's destination base a2 for (vs, variant).
uint32_t ucodeDestVA(uint32_t vs, uint32_t variant);

// Failure reason of the last call on this thread: 0 ok, 1 unmapped read, 2 more than 41
// entries, 3 a write outside `out`.
int lastFailure();
bool assembleVertexFetches(uint32_t vs, uint32_t decl, const uint8_t strides[16], uint32_t variant,
                           uint8_t* out, size_t outBytes, int64_t outBias, uint16_t garbage = 0);
// The program the XDK IM_LOADs for (vs, variant): [a2, a2 + *(container+4)) with
// a2 = ucodeDestVA (GpuEmitDrawState_82108668: address *(hdr+0) + *(vs+32), dwords *(hdr+4)/4).
uint32_t programBytes(uint32_t vs, uint32_t variant);

}  // namespace sr::native::vspatch
