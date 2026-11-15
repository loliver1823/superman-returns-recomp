// R1 batch 2 (docs/SPLICE_CONVERSION.md): host-side ports of 24 FORCING splices. Each group
// reproduces its old hand-edited body exactly (same register / memory effects and control
// transfer on every path, quirks included), proved by the L6 golden-call differential
// (archive/r1/l6/b2.script). The splices' log lines are dropped; every forcing site counts its
// fires with R1_FIRE (kernel/r1_hooks.hpp). The [[midasm_hook]] entries are in
// tools/regen/r1_hooks_b2.toml; the registry entries in archive/r1/reg_b2.json.
//
// Hook semantics (XenonRecomp recompiler.cpp): a hook runs BEFORE its instruction (after the
// label, when the address carries one); after_instruction = true runs it after. Registers come
// by reference; there is no ctx and no base.

#include "r1_hooks.hpp"
#include "guest_context.hpp"
#include <intrin.h>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>


using steady = std::chrono::steady_clock;

// =====================================================================================
// AptCoreImplGetOrCreate_sub_826D4930. Splice (logs dropped): (a) after `lwz r3,9188(r11)`
// (env) it published a non-zero env to sr::kernel::g_aptEnvVA (read only by the compiled-out
// SR_STORE_WATCHPOINTS Apt-env watch; kept, it is not a log); (b) at the env->vt[2] bctrl
// (0x826D4990) a target outside the code range was not called: r3.u32 = 0 (low word only) and
// execution continued at the `cmplwi cr6,r3,0` after the call. The splice also set
// lr = 0x826D4994 on that path; the hook cannot write lr, and lr is dead there (the next
// read is the `bl 0x826885a0` at 0x826D4A54, which overwrites it; the epilogue restores the
// saved one). sub_826D4930 has a kernel override (apt_parse_trace.cpp): midasm only.
// =====================================================================================
void R1_sub_826D4930_826D497C(PPCRegister& r3) {
    if (r3.u32 != 0)
        sr::kernel::g_aptEnvVA.store(r3.u32, std::memory_order_relaxed);
}

// =====================================================================================
// Indirect-call guards retired in RELEASE_PLAN phase 4.3, batch 2 (docs/FORCING_RETIREMENT.md): the
// 'bctrl target outside .text -> skip the call, r3 = 0' hooks are deleted; the generated bctrl runs,
// as on the console (a vtable entry outside the executable is a corrupt object the console jumps
// through; none arose in the census rolls).
// Here: AptCoreImplGetOrCreate (sub_826D4930 @0x826D4990; its Apt-env publish hook @0x826D497C stays),
// RtamResMgrUnload (sub_826E52D0) and AptDispObjRemoveAndDestroy (sub_826E55F8 x2).
// =====================================================================================

// sub_826F7D68 (queue count under the CS): the R1 hook that slept the host thread 100 us on an
// empty queue (iter-184/186's spin throttle, carried by the splice) was retired in RELEASE_PLAN
// phase 4 row 3 (docs/FORCING_RETIREMENT.md): the generated getter returns the count and its
// polling caller runs as it does on the console.

// FmModuleShutdownGraphWalk_sub_8271E6B8 + SystemRunnerShutdownCurrent_sub_8271EA20: the R1
// override and hooks that aborted the shutdown walk 200 ms after SystemRunnerShutdownCurrent armed
// a per-thread timer (the old splice's hang-breaker) were retired in RELEASE_PLAN phase 4 row 4
// (docs/FORCING_RETIREMENT.md): the generated walk runs every node's shutdown to completion, as on
// the console.

// The second splice edit - after the signal, jump past the WaitForEnd sub_825F71F8 join and its
// argument setup to loc_827206A4 - was retired in RELEASE_PLAN phase 4 row 5
// (docs/FORCING_RETIREMENT.md): the generated code joins the loop thread, as on the console.

// sub_82755A38 (descriptor list teardown): the R1 hook that skipped the final list-head clear
// `stw r11,12(r28)` for objects below 0x82000000 (every heap object) was retired in RELEASE_PLAN
// phase 4 row 6 (docs/FORCING_RETIREMENT.md): the generated store runs, as on the console; its
// sole caller frees the object on the next line.

// =====================================================================================
// Lua guards retired in RELEASE_PLAN phase 4.3, batch Lua (docs/FORCING_RETIREMENT.md): the hooks
// on lua_settop (sub_8275BDB8), luaS_resize (sub_827605F8, with its null-L entry override),
// luaM_realloc (sub_82760928), luaD_throw (sub_827623B8), ldo_correctstack (sub_82762480),
// luaD_precall (sub_82762B10, the iter-318h SR_VM_FRAME_CLAMP), lgc_traversestack (sub_82763C50)
// and the two per-call 500000-step caps of lgc_cleartablevalues (sub_82763EE8) are deleted:
// the generated bodies run, as on the console. The luaD_reallocstack hook @0x8276267C stays
// (release/hooks owns it).
// =====================================================================================

// =====================================================================================
// luaD_growstack (luaD_reallocstack) sub_82762640: no hook left. R1 ported two splice edits:
//  - a guard before `bl luaM_realloc` (0x8276267C): a new size over 16 MB or an old stack in
//    0x20202020..0x7F7F7F7F skipped the realloc and emptied the stack. Deleted (iter-467 D3,
//    docs/SPLICE_CONVERSION.md): a valid lua_State's stack is a block of the script heap
//    (its DOGMA pool, or the pool's backing arena above 4 KB), which on Xenia lies in
//    0x00A1xxxx-0x00B3xxxx and here next to our lua_State at 0x00C535C4; 16 MB is 2M slots,
//    beyond 4096 CallInfos (LUA_MAXCALLS, luaD_growCI) of 250-slot frames. The guard only ever
//    caught corrupted state, and a heap in 0x20202020..0x7F7F7F7F (Xenia's 0x40000000+ heap,
//    our 0x70000000+ one) would make it fire on a legitimate stack;
//  - the dropped `mr r4,r29` (0x82762690), reproduced by a hook that D3 deleted first.
// =====================================================================================

// =====================================================================================
// Corrupt-state guards and caps retired in RELEASE_PLAN phase 4.3, batch 3 (docs/FORCING_RETIREMENT.md):
// the generated bodies run, as on the console; the triggers (the pre-regen allocator, locking and
// null-renderer era, docs/BANDAIDS.md 2026-02..04 and the iter-126/148/189 guards) are gone.
// Here: the DOGMA hash probe cap (sub_825FD958), the FmFileReq null stream vtable (sub_82606328), the
// ServiceInitGraph_AddRefNodes depth override (sub_8271E5B0), the FmModuleInitGraphWalk cycle/depth override
// and its 2048-entry cap (sub_8271E630), the null runner node (sub_8271ED68) and the null loop thread
// (sub_82720650).
// =====================================================================================

// =====================================================================================
// Platform / timing guards retired in RELEASE_PLAN phase 4.3, batch 4 (docs/FORCING_RETIREMENT.md): the
// platform part each one stood in for is real now (the live command processor, the 50 MHz timebase, the
// condvar's own semaphore), so the generated code runs, as on the console.
// Here: SemWaitUntilDeadline (sub_825FC1A0 x2, the host-inserted CreateSemaphore) and FmDevice_WaitRequestDone
// (sub_82600E38: the 10 s host wall clock and the override that armed it).
// =====================================================================================
