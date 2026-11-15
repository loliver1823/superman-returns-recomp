#pragma once

// Step 4 item 7a (lane D, 2026-10-20): the host's own HLSL (the frame renderer's transfer / EDRAM / scan-out passes,
// the overlay, the present chain, the costume viewer) compiled AHEAD OF TIME.
//
// Why: the first PM4 frame renders synchronously on the CP, and its lazy one-time setup compiled these shaders with
// D3DCompile (FXC, -O3) at first use - 64-78 % of the CP's time in the boot -> intro hand-off second (profiler boots
// P1 / P2, archive/step4_claude_20261018/timing NOTES 2026-10-20 02:32), the ~0.9 s the FMV state machine's lock is
// held across BlockUntilIdle's fence wait.
//
// tools/regen/host_hlsl_aot.py compiles every (source, entry, target, defines, flags) the code compiles, at build time,
// with the same D3DCompile (d3dcompiler_47), into <build>/generated/host_hlsl_blobs*.cpp; each generated unit registers
// its table here at static init. hostHlslCompile() has D3DCompile's contract: with SR_HOST_HLSL_AOT=1 it returns the
// embedded blob whose key (an FNV-1a 64 of the name, entry, target, flags, defines and the exact source bytes) matches,
// and only when none does compiles at runtime as before - logged once per key ([HOST_HLSL] runtime compile). Default
// (unset / 0): D3DCompile exactly as before, nothing looked up.

#include <d3dcommon.h>
#include <cstddef>
#include <cstdint>

namespace sr::gfx {

struct HostHlslBlob {
  uint64_t key;
  const unsigned char* data;
  uint32_t size;
  const char* label;  // "<name>:<entry>:<target>[:<defines>]"
};

// Called by the generated units' static initializers (the oracle unit's table is absent from a release exe).
bool registerHostHlslTable(const HostHlslBlob* table, uint32_t count);

// The key the build tool computes for the same inputs (tools/regen/host_hlsl_aot.py key()).
uint64_t hostHlslKey(const void* src, size_t len, const char* name, const D3D_SHADER_MACRO* defines,
                     const char* entry, const char* target, unsigned flags1, unsigned flags2);

bool hostHlslAotOn();  // SR_HOST_HLSL_AOT, default ON (baked after the 7a A/B); =0 = runtime D3DCompile (latched)

// D3DCompile's contract (no include handler: none of the host shaders includes a file).
HRESULT hostHlslCompile(const void* src, size_t len, const char* name, const D3D_SHADER_MACRO* defines,
                        const char* entry, const char* target, unsigned flags1, unsigned flags2, ID3DBlob** code,
                        ID3DBlob** errors);

}  // namespace sr::gfx
