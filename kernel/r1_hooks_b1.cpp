// R1 batch 1 (docs/SPLICE_CONVERSION.md): host-side hooks that replace 24 hand-edited
// ("spliced") recompiled bodies. Every hook reproduces its splice EXACTLY (coordinator decision
// D1): the same condition (same widths), the same register and memory writes, the same jump
// target, on every path, including the quirks. Only logging changed: the splices' stderr /
// [DIAG] / [FM_CTOR] / [EXTHREAD_PATH] lines are dropped and each forced fire is counted with
// R1_FIRE. The TOML side is tools/regen/r1_hooks_b1.toml; the registry entries are
// archive/r1/reg_b1.json; the L6 golden-call proof is archive/r1/l6/b1.script.
//
// Per-call locals the splices kept (loop counters) are thread_local here, reset by an entry hook
// on the same path the splice initialised its local. Each function's group says why a nested
// call on the same thread cannot interleave with the counter (the only way a thread_local
// could differ from a per-call local).

#include "r1_hooks.hpp"
#include <cstdint>

// ---------------------------------------------------------------------------------------------
// D3DDeviceBlockUntilIdle (sub_820FDCB8): the hook before 0x820FDD18 that dropped the `bne cr6` back-branch of
// the wait on dev+10868 (0x2A74) is deleted (RELEASE_PLAN phase 4.3 batch 4, the Xenia oracle check in
// docs/FORCING_RETIREMENT.md): the console's pump thread (GpuCommandSegmentEndSync_82110768) brings the count
// back to 0 and our command processor runs the same ISR and pump, so the generated wait runs, as on the console.

// ---------------------------------------------------------------------------------------------
// Indirect-call guards retired in RELEASE_PLAN phase 4.3, batch 2 (docs/FORCING_RETIREMENT.md): the
// 'bctrl target outside .text -> skip the call, r3 = 0' hooks are deleted; the generated bctrl runs,
// as on the console (a vtable entry outside the executable is a corrupt object the console jumps
// through; none arose in the census rolls).

// sub_8229F460 (video-out / net init: the 'conn' -> '+isp' poll): the R1 100-poll cap hooks
// (0x8229F5C4 reset, 0x8229F5CC cap) were retired in RELEASE_PLAN phase 4 row 2
// (docs/FORCING_RETIREMENT.md): the poll ended only by the cap because the generic
// NetDll_XNetGetTitleXnAddr stub answered XNET_GET_XNADDR_PENDING forever; kernel/stubs.cpp now
// answers it as Xenia does and the generated loop ends on '+isp' by itself.

// ---------------------------------------------------------------------------------------------
// SR_ANIM_STUB retired in RELEASE_PLAN phase 4, row 1 (docs/FORCING_RETIREMENT.md): the three hooks that
// skipped the game's D3DX-style texture codec right after each prologue - TextureBoxResample1D
// (sub_823A00C0 @0x823A00D8), TextureBoxResample2D (sub_823A0850 @0x823A0868) and the RGB565 texel writer
// SurfaceWriteTexelsRGB565 (sub_823ACA30 @0x823ACA3C) - and the knob are deleted; the generated bodies run,
// as on the console. The iter-381b/c ">500 s in TextureBoxResample2D" A/B behind the stub never ran
// pristine code; the 2026-10-10 census (base exe, natural_new, stub ON vs SR_ANIM_STUB=0, and the 16.5-min
// tutorial tape) found the two resample kernels entered 0 times on these flows and the writer 30 times
// (3 mip tails of each of 10 decal / LUT / suit textures loaded through TextureResHandler_Load).
// The renderer's iter-450S zero-tail fallback is unchanged.

// ---------------------------------------------------------------------------------------------
// Corrupt-state guards and caps retired in RELEASE_PLAN phase 4.3, batch 3 (docs/FORCING_RETIREMENT.md):
// the generated bodies run, as on the console; the triggers (the pre-regen allocator, locking and
// null-renderer era, docs/BANDAIDS.md 2026-02..04 and the iter-126/148/189 guards) are gone.
// Here: HashMapContainer_dtor (sub_8218F890 x2), the sub_82191D58 tree walk, HashMapFind (sub_821959D0),
// CopyConstructRange96Vec (sub_821DDD18), StlVectorRangeInsert4 (sub_82357E60), ArenaAllocAlignedGrow
// (sub_823AEAB0), XapiCallThreadNotifyRoutines (sub_8246A1D8), RtlpInsertFreeBlock (sub_8246B670), the
// GetProcessHeap override (sub_8246DF08) and the perf-counter divisor (sub_825667C8).

// ---------------------------------------------------------------------------------------------
// Platform / timing guards retired in RELEASE_PLAN phase 4.3, batch 4 (docs/FORCING_RETIREMENT.md): the
// platform part each one stood in for is real now (the live command processor, the 50 MHz timebase, the
// condvar's own semaphore), so the generated code runs, as on the console.
// Here: D3DDeviceCmdBufSegmentBeginReemitState (sub_820F0048), GpuCommandBufferEnd (sub_82111B30 x2).
