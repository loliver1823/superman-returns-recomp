#pragma once

// NATIVE: host implementations of hot XDK D3D leaf functions (docs/NATIVE_ROADMAP.md,
// docs/RECOMP_PERF_REFERENCE.md rank 5 "the D3D leaf is native's"). A leaf here is
// a PC implementation of a library function with the SAME guest-visible effect
// (memory and the registers the ABI keeps), so the game still makes every decision:
// a platform fix in the UnleashedRecomp sense, not forcing.
//
//   SR_NATIVE_CONST_LEAF=0        run the recompiled guest bodies (A/B); default ON
//   SR_NATIVE_CONST_LEAF_CHECK=1  per call: native, snapshot, then the guest body on
//                                 the same arguments, compare the written bytes and
//                                 the dirty word ([NATIVE_LEAF] counters + examples)
//
// Leaves: D3DDevice_SetVertexShaderConstantF_sub_820F4920 (dev+0x700, dirty dev+0)
// and D3DDevice_SetPixelShaderConstantF_sub_820F4A30 (dev+0x1700, dirty dev+8):
// copy `count` vec4 (16 bytes each, unaligned source, lvlx/lvrx pairs) to the
// shadow and OR the caller's 64-bit mask r7 into the dirty word.
//
// NATIVE N1 phase 1 (native_leaves.cpp, end): ShaderPs/Vs_UploadAllConstants (8260B660 /
// 8260B2A0), ~89% of those leaf calls, run their F list as host code.
//   SR_NATIVE_CONST_UPLOAD=0        the guest bodies (A/B); default ON: native F list, I/B lists
//                                   + callbacks stay guest calls
//   SR_NATIVE_CONST_UPLOAD_CHECK=1  per call: native F pass on a copy, guest body for real,
//                                   compare its F calls + the shadow after them ([NATIVE_UPLOAD])
//
// C1: the CHECKs and the sizing diagnostics are development oracles, compiled out under SR_ORACLE=0
// (kernel/sr_oracle.hpp; native_leaves.cpp). SR_NATIVE_LEAF_CALLERS' API below is then a constant-false shim.

#include <cstdint>

#include "sr_oracle.hpp"

struct PPCContext;

namespace sr::native::leaf {
// NATIVE N1 sizing (SR_NATIVE_LEAF_CALLERS): the host return address of the leaf's guest caller,
// set by the override wrappers before they call setShaderConstantF.
#if SR_ORACLE
extern thread_local const void* t_leafCaller;
extern const bool g_leafCallersOn;
#else
// SR_ORACLE=0: the sizing diagnostic is compiled out; a wrapper's `if (g_leafCallersOn) t_leafCaller = ...` sees a
// constant false (as with SR_NATIVE_LEAF_CALLERS unset) and never stores.
constexpr bool g_leafCallersOn = false;
inline thread_local const void* t_leafCaller = nullptr;
#endif

// Returns after doing the work natively, or after delegating to `guestBody`
// when an argument is outside the native fast path (e.g. a range that is not
// contiguous in host memory). `vsNotPs` selects the shadow and dirty word.
void setShaderConstantF(PPCContext& ctx, uint8_t* base, bool vsNotPs,
                        void (*guestBody)(PPCContext&, uint8_t*));

// GpuEmitType0RegPacketsVec_sub_821078C0: the XDK draw flush's constant upload into the
// command buffer (8.5-10.5% of the gameplay render thread). SR_NATIVE_TYPE0_LEAF=0 = guest
// body; SR_NATIVE_TYPE0_LEAF_CHECK=1 = per call native, rewind the cursor, guest body,
// compare the cursor and every written byte.
void type0Dispatch(PPCContext& ctx, uint8_t* base, void (*guestBody)(PPCContext&, uint8_t*));

}  // namespace sr::native::leaf
