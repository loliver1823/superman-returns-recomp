#pragma once
// iter-460: live PM4 trace writer - one frame of the LIVE game as a Xenia
// .xtr (trace_protocol.h format, uncompressed), so Xenia's own
// xenia-gpu-d3d12-trace-dump.exe renders exactly what our renderer consumed.
//
//   SR_PM4_TRACE_SWAP=<k>      record the frame between CP swap k-1 and swap k
//   SR_PM4_TRACE_DIR=<dir>     output directory (default archive/live_xtr)
//   SR_PM4_TRACE_HOTKEY=1      (iter-462) F9 in the game window records the
//                              frame after the next swap, up to 32 per run, so
//                              a scene reached by playing can be traced. Armed
//                              for the whole run: the render thread stays off.
//   SR_PM4_TRACE_FULL=0        (iter-463, default ON) record only what the
//                              renderer read. ON: every draw also records its
//                              IB, vertex fetch ranges and the full base + mip
//                              backing of every texture fetch constant, before
//                              the shader gates (rejected draws included), and
//                              pages validated before the trace opened
//                              (D3D12Backend::pm4TraceDrawBacking), each from
//                              the window validation serves it from (physwin
//                              owner). Pages resolved earlier in the frame are
//                              never recorded from the CPU copy; a GPU-written
//                              page only once the CPU rewrote it.
//   SR_PM4_TRACE_FRAMES=<n>    (p2.zpd zpdx3, default 1) every trace holds n
//                              consecutive frames: a swap event after each
//                              frame's swap packet (where Xenia's own writer
//                              puts it), each frame's loose reads first in
//                              that frame. Xenia's trace-dump plays frame 0
//                              only; the zpd oracle build plays every frame
//                              (tools/xenia-trace/xenia_zpd_oracle.patch).
//
// A packet the engine executes from a HOST copy - the CP copies every primary
// ring packet out of the ring (kernel/gpu_command_processor.cpp) - has no
// guest physical address: it is recorded at kHostPacketPhys, the top MiB of
// physical memory, which no guest allocation reaches (the physical arena ends
// at VA 0xBF000000 = phys 0x1F000000). Xenia copies a packet to its address at
// PacketStart and executes it from there at PacketEnd, so one address serves
// them all (p2.zpd zpdx2: recorded at 0, the packets were overwritten by reads
// of physical page 0 and Xenia's dump crashed).
//
// Format and nesting follow xenia-canary src/xenia/gpu/trace_writer.cc and
// pm4_command_processor_implement.h (BSD-3-Clause, Xenia Project):
// - every packet is PacketStart{phys, dwords} + PacketEnd; an INDIRECT_BUFFER
//   packet is recorded with 2 dwords, its packets follow individually;
// - memory the GPU reads is a MemoryRead inside the packet that reads it, so
//   the player writes it before executing the packet;
// - the file opens with the register file, the display gamma ramp, the EDRAM
//   and the GPU-written memory (Xenia InitializeTrace).
// Our renderer reads textures and vertices at DRAIN / PRESENT time, after the
// packets were executed; those reads carry the draw's ordinal and are placed
// inside that draw's packet when the file is written.
#include <cstddef>
#include <cstdint>
#include <vector>

#include "../kernel/sr_oracle.hpp"

namespace sr::gfx::livetrace {

constexpr uint32_t kHostPacketPhys = 0x1FF00000u;  // where host-side packets are recorded (see above)

#if SR_ORACLE
// Cheap checks for the hot paths: armed = SR_PM4_TRACE_SWAP is set (the
// engine then reports packets so swaps are counted); recording = inside the
// traced frame.
bool armed();
bool recording();

// Engine (CP thread). `host` = the packet's first dword in guest memory.
void packetBegin(const uint8_t* host, uint32_t dwords);
// drawOrdinal: the engine's draw count if this packet was a draw, else 0.
void packetEnd(uint32_t drawOrdinal, bool isSwap);

// A read of guest memory at physical `phys`. Attached to the current draw
// context if one is set, else to the open packet, else appended.
void memRead(uint32_t phys, const uint8_t* host, uint32_t bytes);

// NATIVE mode B 1a: the physical address of a guest-memory host pointer, and bytes for the
// trace's ".nrt" sidecar (written next to the .xtr when the frame closes).
uint32_t hostPhys(const uint8_t* host);
void sidecarAppend(const void* data, size_t bytes);

// Host input (SR_PM4_TRACE_HOTKEY=1): record the frame after the next swap.
void requestCapture();

// p2.zpd zpd7 (the Xenia oracle, docs/NATIVE_ROADMAP.md P2.12 item 14), CP thread: an occlusion query began.
// SR_PM4_TRACE_ZPD=<k> (with SR_PM4_TRACE_HOTKEY=1): request the frame after the next swap, as the hotkey does, up to
// k times and at least SR_PM4_TRACE_ZPD_GAP swaps apart (default 600), so a roll whose HUD queries traces k frames
// that hold them. A no-op without both.
void requestOnQuery();
// The swap number of the trace being recorded (its file is live_454107ED_<swap>.xtr: the first frame's swap, for every
// frame of a multi-frame trace), 0 when none is.
uint32_t tracedSwap();

// Renderer: the draw whose reads follow (0 = none).
void setDrawContext(uint32_t drawOrdinal);
#if defined(SR_DIFFTEST)
// difftest only (tools/difftest/native_zpd_test.cpp Z19): the switches as SR_PM4_TRACE_HOTKEY / SR_PM4_TRACE_FRAMES /
// SR_PM4_TRACE_DIR would set them, every swap / request counter and the open trace reset.
void configureForTest(bool hotkey, long framesPerTrace, const char* dir);
#endif
#else
// SR_ORACLE=0 (kernel/sr_oracle.hpp): the live trace writer is compiled out (an unset SR_PM4_TRACE_SWAP /
// SR_PM4_TRACE_HOTKEY: never armed, never recording, every tap a no-op).
inline bool armed() { return false; }
inline bool recording() { return false; }
inline void packetBegin(const uint8_t*, uint32_t) {}
inline void packetEnd(uint32_t, bool) {}
inline void memRead(uint32_t, const uint8_t*, uint32_t) {}
inline uint32_t hostPhys(const uint8_t*) { return 0; }
inline void sidecarAppend(const void*, size_t) {}
inline void requestCapture() {}
inline void requestOnQuery() {}
inline uint32_t tracedSwap() { return 0; }
inline void setDrawContext(uint32_t) {}
#endif

// Opening the trace asks the renderer for the initial GPU state.
struct InitState {
  std::vector<uint8_t> edram;  // 10 MiB, Xenia EDRAM snapshot layout (empty = none)
  // GPU-written memory: (phys, bytes) pairs, bytes as the GPU holds them.
  std::vector<std::pair<uint32_t, std::vector<uint8_t>>> gpuPages;
};
using InitProvider = void (*)(InitState& out);
#if SR_ORACLE
void setInitProvider(InitProvider fn);
#else
inline void setInitProvider(InitProvider) {}
#endif

}  // namespace sr::gfx::livetrace
