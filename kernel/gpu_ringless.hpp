#pragma once

// iter-467 native-a P2-A (docs/NATIVE_ROADMAP.md "P2-A design"): the host IB queue, gate
// SR_NATIVE_RINGLESS (default ON since the P2-A proof of 2026-10-09; =0 keeps the ring path; in-roll A/B
// SR_AB_TOGGLE=SR_NATIVE_RINGLESS, per present with SR_AB_PRESENT=1).
//
// The two ring writers are replaced by host code:
//   GpuRingUpdateWritePointer_820FD218(dev, desc {dwords, phys}) - the XDK's own space wait
//   (820FD150, which reads the read index fb+0x3C the consumer writes), then the INDIRECT_BUFFER
//   descriptor goes to the CP thread's item queue (gpuCpPushItem) instead of ring memory; dev+0x2A44
//   advances by 3 mod mask exactly as the ring write would; the publish (gpuCpDoorbell) replaces the
//   CP_RB_WPTR MMIO store.
//   GpuRingEmitBinSelectLo_82110140(S) - the Z-pass bin select the pump writes straight into the ring:
//   the value and the S+0x16C cache as the XDK computes them, the space wait, then a BS item at the
//   ring index (2 dwords), no publish (the XDK rings no doorbell for it either).
// The CP loop consumes ring index space in order: at an index a host item occupies it takes the item
// (IB -> executeGuestIndirectBuffer, BS -> applyBinReg), else it parses the ring packet there, so ring
// presents and ringless presents interleave with no drain. Modes the guest body keeps whole: no-GPU
// (byte dev+0x2A39 & 2) and PIX attached (dev+0x4DF4 / dev+0x4E84 non-zero).

#include <cstdint>

struct PPCContext;

namespace sr::kernel::ringless {

bool gateOn();  // SR_NATIVE_RINGLESS latched at load
// The 820FD218 body under the gate (and the A/B toggle). false = not taken: the caller runs the guest
// body and the doorbell observer as before.
bool updateWritePointer(PPCContext& ctx, uint8_t* base);
void finalLine(const char* why);
// tools/graceful-quit (kernel/quit.hpp): IB + bin-select items pushed so far (the quit's settle digest).
uint64_t itemsTotal();

}  // namespace sr::kernel::ringless
