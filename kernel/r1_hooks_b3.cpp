// R1 batch 3 (docs/SPLICE_CONVERSION.md): the host side of the [[midasm_hook]] entries in
// tools/regen/r1_hooks_b3.toml. Each group below replaces one hand-edited ("spliced")
// recompiled body; the hook reproduces the splice's register and memory effects and its
// control transfer exactly, on every path. Only logging changed: the splices' log lines are
// gone and every forced path counts its fires with R1_FIRE (kernel/r1_hooks.hpp).
//
// All 23 functions use midasm hooks, none a strong-def override. Several are Lua internals
// that call each other intra-TU (ENGINE_MAP 6c: a strong def never fires there), three already
// have an override (sub_82764348, sub_827657C8, sub_8279EC60), and a hook inside __imp__ runs
// whoever the caller is.
//
// Per-invocation state. Three splices kept a C++ local of the spliced function (a counter, a
// saved list head, a deadline), which lives once per CALL of the function. A hook has no
// frame of its own, so that state lives in FrameSlots: a per-thread map keyed by the guest
// frame (r1 after the function's stwu, which is unique to the invocation while it is live and
// constant through its body). Every invocation writes its slot before it reads it, exactly as
// the splice initialised its local, so a stale slot left by a returned (or unwound) call at
// the same address is never observed. Nothing is ever popped, so a frame on another stack
// (fiber) keeps its slot. The last frame is cached, so the per-instruction hook in
// luaV_execute costs one compare on the hot path.

#include "r1_hooks.hpp"
#include "guest_context.hpp"

#include <chrono>
#include <cstdint>
#include <unordered_map>

namespace {

template <typename T>
class FrameSlots {
public:
    T& at(uint32_t frame) {
        if (cached_ != nullptr && frame == cachedFrame_)
            return *cached_;
        T& v = slots_[frame];   // value-initialised on first use; references stay valid
        cachedFrame_ = frame;
        cached_ = &v;
        return v;
    }

private:
    uint32_t cachedFrame_ = 0;
    T* cached_ = nullptr;
    std::unordered_map<uint32_t, T> slots_;
};

}  // namespace

// ---------------------------------------------------------------------------------------------
// sub_82764348 luaC_callGCTM_sub_82764348 (Lua 5.0 luaC_callGCTM: while (G->tmudata) do1gcTM):
// the R1 hook that cut the finalizer loop every 50000 cumulative iterations per thread (the old
// splice's per-call reset wrote a shadowing variable) was retired in RELEASE_PLAN phase 4 row 8
// (docs/FORCING_RETIREMENT.md): every pending __gc finalizer runs in one call, as on the console.

// ---------------------------------------------------------------------------------------------
// Lua guards retired in RELEASE_PLAN phase 4.3, batch Lua (docs/FORCING_RETIREMENT.md): the hooks
// on lgc_mark (sub_82764510), luaT_gettmbyobj (sub_82764760), luaV_execute (sub_827657C8),
// luaO_pushvfstring (sub_82766C78), luaH_getstr (sub_82767DF0), ltable newkey (sub_827684C0),
// luaZ_openspace (sub_82768878) and luaG_typeerror (sub_8276A4B0) are deleted: the generated
// bodies run, as on the console (a corrupt state they guarded against is one the console reads
// through or loops on; none arose in the census rolls).

// ---------------------------------------------------------------------------------------------
// Indirect-call guards retired in RELEASE_PLAN phase 4.3, batch 2 (docs/FORCING_RETIREMENT.md): the
// 'bctrl target outside .text -> skip the call, r3 = 0' hooks are deleted; the generated bctrl runs,
// as on the console (a vtable entry outside the executable is a corrupt object the console jumps
// through; none arose in the census rolls).
// Here: sub_82773958 (singleton init + vt[0] call) @0x827739C4.

void R1_sub_827A1698_827A16C0(PPCRegister& r28)
{
    R1_FIRE("sub_827A1698@0x827A16C0 doexit skips CRT termination", r28.u32, 0);
}

void R1_sub_827A1698_827A1788(PPCRegister& r11)
{
    R1_FIRE("sub_827A1698@0x827A1788 skip KeBugCheck(0) for retcaller 0", r11.u32, 0);
}

// ---------------------------------------------------------------------------------------------
// sub_827A55F8 Crt_getptd_noexit_sub_827A55F8.
// Splice: `static thread_local int s_depth`; at entry, before the prologue: if ++s_depth > 2
// { --s_depth; r3.u64 = 0; return; }; on the normal return, after `mr r3,r31` and before
// `addi r1,r1,112`, --s_depth. Nothing else decrements it: a longjmp/throw unwinding through the
// body leaves it incremented, in the splice and here alike (no RAII). Hooks: 0x827A55F8
// return_on_true, 0x827A56AC void.

// ---------------------------------------------------------------------------------------------
// sub_828CCE60 (DOGMA heap: drain the two pending queues under the heap lock).
// Splice: block-local `queuePtrBefore = r11` after the queue-1 head test (fall-through of the beq
// at 0x828CCEE4), and after the vt[144] dispatch: obj = [r1+132]; if [obj+1196] still equals it,
// [obj+1196] = obj+1184 (the sentinel: queue emptied). Same for queue 2 with `q2Before` (after
// the beq at 0x828CCF40), vt[188]+vt[164], [obj+1128] and obj+1100. The locals are per call
// (the dispatches can re-enter on this thread), so they live in FrameSlots keyed by r1 (frame
// after stwu -112). Hooks: 0x828CCEE8 / 0x828CCF44 (save), 0x828CCF28 / 0x828CCF8C (check,
// before the `b` back to the loop head; neither address is a label).

// ---------------------------------------------------------------------------------------------
// sub_828D0968 (DOGMA heap list pass under the heap lock; returns the total in r24).
// Splice: at C++ entry (before the prologue) deadline = steady_clock::now() + 1000 ms; at
// loc_828D09A0 (label first: first entry and every bne back) if now > deadline goto
// loc_828D0AA4 (release the lock, return r24); fprintf dropped. Its `timed_out` flag is always
// false when tested (a fire leaves the loop for good), so it changes nothing. The deadline is
// per call: an entry hook at 0x828D0968 stores it in FrameSlots under the frame the call is
// about to make (r1 - 176, the stwu below), read back at 0x828D09A0 under r1.

// ---------------------------------------------------------------------------------------------
// Corrupt-state guards and caps retired in RELEASE_PLAN phase 4.3, batch 3 (docs/FORCING_RETIREMENT.md):
// the generated bodies run, as on the console; the triggers (the pre-regen allocator, locking and
// null-renderer era, docs/BANDAIDS.md 2026-02..04 and the iter-126/148/189 guards) are gone.
// Here: EventListener_UnsubscribeAll (sub_82774230), RenderBeginPickBuffer (sub_82787468), DogmaFree
// (sub_8278D3C0, sub_8278D400), memcpy > 16 MB (sub_8279EC60), Crt_getptd_noexit (sub_827A55F8),
// _mtinitlocknum (sub_827AC358), NewBackendExpandNativeAluOpsPass (sub_8280DDC0), the DOGMA queue drain
// (sub_828CCE60) and the DOGMA realloc host-stack guard (sub_828CF878).

// ---------------------------------------------------------------------------------------------
// Platform / timing guards retired in RELEASE_PLAN phase 4.3, batch 4 (docs/FORCING_RETIREMENT.md): the
// platform part each one stood in for is real now (the live command processor, the 50 MHz timebase, the
// condvar's own semaphore), so the generated code runs, as on the console.
// Here: CrtDoexit's two isGuestVAInRange(r31+164) guards (0x827A16B0, 0x827A177C; r31 is the CRT's own static
// block) and the DOGMA list pass 1 s deadline (sub_828D0968). CrtDoexit's termination bypass (0x827A16C0) and
// KeBugCheck skip (0x827A1788) STAY as a documented platform fix: on PC the host ends the process; the console
// runs its onexit table and terminators and hands off to the dashboard through KeBugCheck(0).
