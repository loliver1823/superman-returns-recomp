#pragma once
// iter-466 cp lane: SSE4.1 kernels for the command processor's hot copy loops (gfx/cp_simd.cpp,
// compiled with -msse4.1, chosen at run time). Every function returns the scalar loop's result
// exactly; callers keep the scalar loop as the fallback (no SSE4.1, or the gate off).
#include <cstdint>

namespace sr::gfx::cpsimd {

// An SR_CP_* gate value: unset = def, "0" = off, anything else = on.
bool gate(const char* name, bool def);
// The same, plus one [CP_GATE] log line with the value (the activation witness; defined in
// gfx/d3d12_pm4_edram.cpp, next to the logger).
bool gateLogged(const char* name, bool def);
// SR_CP_VSF_NOWIN (gfx/d3d12_pm4_edram.cpp): on the shared-memory vertex path, skip the host-side
// window choices the in-shader fetch does not use (pm4StageVsf's per-fetch window, the drain's
// pre-decode dedupe key).
bool vsfNoWin();

// SSE4.1 present (CPUID leaf 1: SSSE3 and SSE4.1).
bool available();

// dst[i] = bswap32(load_le32(srcBE + 4 * i)) for i in [0, n): the PM4 type-0 run decode
// (readBE32 per dword).
void bswapCopy32(uint32_t* dst, const uint8_t* srcBE, uint32_t n);

// The PM4 vertex-fetch index remap (d3d12_pm4_edram.cpp pm4StageVsf):
//   r      = the guest index (16-bit: byte-swapped for endian 1 / 2; 32-bit: GpuSwap by endian)
//   dst[i] = (cut && r == resetIndex) ? 0xFFFFFFFF : clampOpt(((r + offset) & 0xFFFFFF))
// clampOpt = min(max(v, minV), maxV) when clamp. Returns the number of indices written (n).
struct RemapParams {
  uint32_t offset, minV, maxV, resetIndex, endian;
  bool clamp, cut, idx32;
};
uint32_t remapIndices(uint32_t* dst, const uint8_t* ib, uint32_t n, const RemapParams& p);

}  // namespace sr::gfx::cpsimd
